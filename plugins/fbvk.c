/* fbvk -- Vulkan + GLFW windowed graphics for fbuild, as a plugin (.so).
 *
 * Build: cc -shared -fPIC -Isrc plugins/fbvk.c -o build/fbvk.so
 * Load:  FBUILD_PLUGINS=build/fbvk.so ./fbuild
 *
 * Neither libvulkan nor libglfw are linked at build time (the generic plugin
 * build recipe -- see .fb / Makefile -- takes no per-plugin -l flags, and
 * every other plugin already follows the "links against nothing but the
 * fbuild host" convention). Both are dlopen'd lazily on first use instead:
 * libvulkan.so.1 for vkGetInstanceProcAddr (the one loader entry point every
 * other Vulkan command is fetched through, volk-style) and libglfw.so.3 for
 * windowing. VK_NO_PROTOTYPES keeps vulkan.h from declaring extern symbols
 * we'd never actually link against.
 *
 * A curated high-level API, not a raw 1:1 binding:
 *
 *   vk.devices()                                 List<Device>  no window needed
 *   vk.app(title, w, h[, opts])                  App           window+instance+
 *                                                                device+swapchain+
 *                                                                renderpass, ready to draw
 *   app.should_close() / .poll()                 Bool / self
 *   app.begin_frame(r,g,b[,a])                    Bool   false = skip (resized/minimized)
 *   app.draw(pipeline, vbuf[, n])                 self
 *   app.draw_indexed(pipeline, vbuf, ibuf[, n])   self
 *   app.end_frame()                               self
 *   app.buffer(list, usage)                       Buffer  usage: vertex|index|uniform
 *   buf.update(list)                               self
 *   app.shader(src_or_path, stage)                Shader  stage: vertex|fragment|compute;
 *                                                          .spv path -> raw bytes, else
 *                                                          compiled via `glslc` on the fly
 *   app.pipeline(vert, frag, layout[, opts])      Pipeline  layout: e.g. List<3,3> = vec3+vec3
 *   app.destroy()                                  Bool
 *
 * v1 scope: single untextured/unlit color-per-vertex pipeline (no depth
 * buffer, no descriptor sets/uniforms bound into the pipeline yet, no MSAA).
 * Real window, real swapchain w/ resize handling, real double-buffered
 * present loop, real runtime-compiled GLSL -- just no textures/3D lighting
 * in this first cut.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "fbuild.h"

/* ---- Vulkan command table (X-macro'd; one copy per instance, loaded via
 * that instance's own vkGetInstanceProcAddr -- see the big comment on
 * load_fns below for why that matters). ------------------------------- */
#define VK_FN_LIST(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceFeatures) \
	X(vkGetPhysicalDeviceFormatProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
	X(vkCreateDevice) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDestroySurfaceKHR) \
	X(vkCreateSwapchainKHR) \
	X(vkDestroySwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkFreeCommandBuffers) \
	X(vkResetCommandBuffer) \
	X(vkResetCommandPool) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) \
	X(vkCmdDraw) \
	X(vkCmdDrawIndexed) \
	X(vkCmdFillBuffer) \
	X(vkCmdCopyBuffer) \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkGetFenceStatus) \
	X(vkAcquireNextImageKHR) \
	X(vkQueueSubmit) \
	X(vkQueuePresentKHR) \
	X(vkDeviceWaitIdle) \
	X(vkQueueWaitIdle) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkBindBufferMemory) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkEnumerateInstanceVersion) \
	X(vkEnumerateInstanceLayerProperties)

typedef struct {
#define X(n) PFN_##n n;
	VK_FN_LIST(X)
#undef X
} vk_fns_t;

/* Every member here is fetched via vkGetInstanceProcAddr(inst, name) --
 * including the device-level ones. The Vulkan loader (libvulkan.so.1, which
 * is what we dlopen -- never an ICD directly) always hands back its own
 * trampoline for device-level commands regardless of which instance's
 * vkGetInstanceProcAddr you went through; the trampoline dispatches off the
 * VkDevice handle's own embedded dispatch table. So a vk_fns_t loaded from
 * instance A's pointer is safe to call with a VkDevice/VkQueue/etc created
 * under instance B. (vkGetDeviceProcAddr would be marginally faster for the
 * device-level half, but doubles the loading code for no behavioral gain
 * in a build-tool plugin.) Kept per-instance anyway (not a shared global)
 * so a query-mode instance's vkDestroyInstance can never be confused with
 * an app-mode one's. */
static void load_fns(vk_fns_t *T, VkInstance inst, PFN_vkGetInstanceProcAddr gipa) {
#define X(n) T->n = (PFN_##n)gipa(inst, #n);
	VK_FN_LIST(X)
#undef X
}

/* ---- GLFW command table (process-global: GLFW has no per-instance concept) */
typedef int            (*PFN_glfwInit)(void);
typedef void            (*PFN_glfwTerminate)(void);
typedef void            (*PFN_glfwWindowHint)(int, int);
typedef GLFWwindow *    (*PFN_glfwCreateWindow)(int, int, const char *, GLFWmonitor *, GLFWwindow *);
typedef void            (*PFN_glfwDestroyWindow)(GLFWwindow *);
typedef void            (*PFN_glfwPollEvents)(void);
typedef int             (*PFN_glfwWindowShouldClose)(GLFWwindow *);
typedef void            (*PFN_glfwSetWindowShouldClose)(GLFWwindow *, int);
typedef void            (*PFN_glfwGetFramebufferSize)(GLFWwindow *, int *, int *);
typedef VkResult         (*PFN_glfwCreateWindowSurface)(VkInstance, GLFWwindow *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef const char **   (*PFN_glfwGetRequiredInstanceExtensions)(uint32_t *);
typedef int              (*PFN_glfwVulkanSupported)(void);
typedef void             (*PFN_glfwSetWindowTitle)(GLFWwindow *, const char *);

typedef struct {
	PFN_glfwInit                          Init;
	PFN_glfwTerminate                     Terminate;
	PFN_glfwWindowHint                    WindowHint;
	PFN_glfwCreateWindow                  CreateWindow;
	PFN_glfwDestroyWindow                 DestroyWindow;
	PFN_glfwPollEvents                    PollEvents;
	PFN_glfwWindowShouldClose             WindowShouldClose;
	PFN_glfwSetWindowShouldClose          SetWindowShouldClose;
	PFN_glfwGetFramebufferSize            GetFramebufferSize;
	PFN_glfwCreateWindowSurface           CreateWindowSurface;
	PFN_glfwGetRequiredInstanceExtensions GetRequiredInstanceExtensions;
	PFN_glfwVulkanSupported               VulkanSupported;
	PFN_glfwSetWindowTitle                SetWindowTitle;
} glfw_fns_t;

static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkCreateInstance      g_create_instance;
static glfw_fns_t                g_glfw;
static int                       g_loader_ready;
static int                       g_glfw_init_done;

static int ensure_loader(fb_State *S) {
	if (g_loader_ready) return 1;

	void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!vk) vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!vk) { fb_error(S, "vk: cannot dlopen libvulkan.so.1: %s", dlerror()); return 0; }
	g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
	if (!g_gipa) { fb_error(S, "vk: libvulkan has no vkGetInstanceProcAddr"); return 0; }
	g_create_instance = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
	if (!g_create_instance) { fb_error(S, "vk: cannot resolve vkCreateInstance"); return 0; }

	void *gl = dlopen("libglfw.so.3", RTLD_NOW | RTLD_LOCAL);
	if (!gl) gl = dlopen("libglfw.so", RTLD_NOW | RTLD_LOCAL);
	if (!gl) { fb_error(S, "vk: cannot dlopen libglfw.so.3: %s", dlerror()); return 0; }
#define LOAD(field, name) \
		g_glfw.field = (PFN_##name)dlsym(gl, #name); \
		if (!g_glfw.field) { fb_error(S, "vk: libglfw missing %s", #name); return 0; }

	LOAD(Init, glfwInit)
	LOAD(Terminate, glfwTerminate)
	LOAD(WindowHint, glfwWindowHint)
	LOAD(CreateWindow, glfwCreateWindow)
	LOAD(DestroyWindow, glfwDestroyWindow)
	LOAD(PollEvents, glfwPollEvents)
	LOAD(WindowShouldClose, glfwWindowShouldClose)
	LOAD(SetWindowShouldClose, glfwSetWindowShouldClose)
	LOAD(GetFramebufferSize, glfwGetFramebufferSize)
	LOAD(CreateWindowSurface, glfwCreateWindowSurface)
	LOAD(GetRequiredInstanceExtensions, glfwGetRequiredInstanceExtensions)
	LOAD(VulkanSupported, glfwVulkanSupported)
	LOAD(SetWindowTitle, glfwSetWindowTitle)
#undef LOAD

	g_loader_ready = 1;
	return 1;
}

/* ---- classes ------------------------------------------------------------ */
static fb_Class VK_CLS, DEVICE_CLS, APP_CLS, BUFFER_CLS, SHADER_CLS, PIPELINE_CLS;

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct {
	GLFWwindow            *win;
	VkInstance             instance;
	VkSurfaceKHR           surface;
	VkPhysicalDevice       phys;
	VkDevice               dev;
	uint32_t               qfam;
	VkQueue                queue;
	vk_fns_t               fn;

	VkSwapchainKHR          swapchain;
	VkFormat                swap_format;
	VkExtent2D              extent;
	uint32_t                image_count;
	VkImage                *images;
	VkImageView            *views;
	VkFramebuffer          *fbs;
	VkSemaphore            *render_done;   /* one per swapchain image */

	VkRenderPass            renderpass;
	VkCommandPool           cmdpool;
	VkCommandBuffer         cmdbufs[MAX_FRAMES_IN_FLIGHT];
	VkSemaphore             img_avail[MAX_FRAMES_IN_FLIGHT];
	VkFence                 in_flight[MAX_FRAMES_IN_FLIGHT];

	uint32_t                frame;         /* rotates 0..MAX_FRAMES_IN_FLIGHT-1 */
	uint32_t                cur_image;     /* swapchain image acquired this frame */
	int                     frame_active;  /* between begin_frame/end_frame */
	int                     width, height;
} vkapp_t;

typedef struct {
	vkapp_t        *app;
	VkBuffer         buf;
	VkDeviceMemory   mem;
	void            *mapped;
	VkDeviceSize     size;
	int              count;    /* vertices (vertex buf) or indices (index buf) */
	int              is_index;
} vkbuf_t;

typedef struct {
	vkapp_t              *app;
	VkShaderModule         mod;
	VkShaderStageFlagBits  stage;
} vkshader_t;

typedef struct {
	vkapp_t         *app;
	VkPipeline        pipeline;
	VkPipelineLayout  layout;
} vkpipe_t;

#define P(v) fb_tostr(S, (v))

/* ---- shared instance/device helpers ------------------------------------ */

static VkInstance make_instance(fb_State *S, const char *app_name, int want_surface,
                                 vk_fns_t *out_fn) {
	uint32_t ext_count = 0;
	const char **exts = NULL;
	if (want_surface) {
		exts = g_glfw.GetRequiredInstanceExtensions(&ext_count);
		if (!exts) { fb_error(S, "vk: glfwGetRequiredInstanceExtensions failed (no display?)"); return VK_NULL_HANDLE; }
	}
	VkApplicationInfo ai = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = app_name,
		.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
		.pEngineName = "fbvk",
		.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &ai,
		.enabledExtensionCount = ext_count,
		.ppEnabledExtensionNames = exts,
	};
	VkInstance inst = VK_NULL_HANDLE;
	VkResult r = g_create_instance(&ci, NULL, &inst);
	if (r != VK_SUCCESS) { fb_error(S, "vk: vkCreateInstance failed (%d)", (int)r); return VK_NULL_HANDLE; }
	if (out_fn) load_fns(out_fn, inst, g_gipa);
	return inst;
}

static const char *devtype_str(VkPhysicalDeviceType t) {
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
	default:                                     return "other";
	}
}

static unsigned long long device_vram(vk_fns_t *fn, VkPhysicalDevice pd) {
	VkPhysicalDeviceMemoryProperties mp;
	fn->vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	unsigned long long vram = 0;
	for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
		if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			vram += mp.memoryHeaps[h].size;
	return vram;
}

/* vk.devices() -> List<Device>: enumerate GPUs without opening a window. */
static fb_Value bi_devices(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	if (!ensure_loader(S)) return fb_nil(S);
	vk_fns_t fn;
	VkInstance inst = make_instance(S, "fbvk-query", 0, &fn);
	if (!inst) return fb_nil(S);

	uint32_t count = 0;
	fn.vkEnumeratePhysicalDevices(inst, &count, NULL);
	VkPhysicalDevice *devs = malloc(sizeof(*devs) * (count ? count : 1));
	fn.vkEnumeratePhysicalDevices(inst, &count, devs);

	fb_Value list = fb_list(S);
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties props;
		fn.vkGetPhysicalDeviceProperties(devs[i], &props);

		fb_Value dev = fb_object(S, DEVICE_CLS, NULL);
		fb_set(S, dev, "index", fb_int(S, (long)i));
		fb_set(S, dev, "name", fb_str(S, props.deviceName));
		fb_set(S, dev, "type", fb_str(S, devtype_str(props.deviceType)));
		fb_set(S, dev, "vram", fb_int(S, (long)device_vram(&fn, devs[i])));

		char verbuf[32];
		snprintf(verbuf, sizeof verbuf, "%u.%u.%u",
		         (unsigned)VK_API_VERSION_MAJOR(props.apiVersion),
		         (unsigned)VK_API_VERSION_MINOR(props.apiVersion),
		         (unsigned)VK_API_VERSION_PATCH(props.apiVersion));
		fb_set(S, dev, "api_version", fb_str(S, verbuf));
		fb_set(S, dev, "driver_version", fb_int(S, (long)props.driverVersion));

		fb_Value limits = fb_map(S);
		fb_set(S, limits, "max_image_2d", fb_int(S, props.limits.maxImageDimension2D));
		fb_set(S, limits, "max_compute_wg_invocations", fb_int(S, props.limits.maxComputeWorkGroupInvocations));
		fb_set(S, limits, "max_push_constants", fb_int(S, props.limits.maxPushConstantsSize));
		fb_set(S, limits, "max_bound_descriptor_sets", fb_int(S, props.limits.maxBoundDescriptorSets));
		fb_set(S, dev, "limits", limits);

		VkPhysicalDeviceFeatures feats;
		fn.vkGetPhysicalDeviceFeatures(devs[i], &feats);
		fb_Value features = fb_map(S);
		fb_set(S, features, "geometry_shader", fb_bool(S, feats.geometryShader));
		fb_set(S, features, "tessellation_shader", fb_bool(S, feats.tessellationShader));
		fb_set(S, features, "wide_lines", fb_bool(S, feats.wideLines));
		fb_set(S, features, "large_points", fb_bool(S, feats.largePoints));
		fb_set(S, features, "sampler_anisotropy", fb_bool(S, feats.samplerAnisotropy));
		fb_set(S, features, "multi_viewport", fb_bool(S, feats.multiViewport));
		fb_set(S, features, "fill_mode_non_solid", fb_bool(S, feats.fillModeNonSolid));
		fb_set(S, features, "shader_float64", fb_bool(S, feats.shaderFloat64));
		fb_set(S, dev, "features", features);

		fb_push(S, list, dev);
	}
	free(devs);
	fn.vkDestroyInstance(inst, NULL);
	return list;
}

/* vk.api_version() -> String: the Vulkan loader/instance API version
 * ("major.minor.patch"), e.g. what vkCreateInstance's apiVersion field can
 * safely request. No window needed -- same throwaway-instance trick as
 * vk.devices() above (vkEnumerateInstanceVersion is a global-level command,
 * but the real Vulkan Loader resolves it the same way through any valid
 * instance's vkGetInstanceProcAddr, same as the device-level commands do --
 * see load_fns's comment). */
static fb_Value bi_api_version(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	if (!ensure_loader(S)) return fb_nil(S);
	vk_fns_t fn;
	VkInstance inst = make_instance(S, "fbvk-query", 0, &fn);
	if (!inst) return fb_nil(S);

	uint32_t ver = 0;
	VkResult r = fn.vkEnumerateInstanceVersion(&ver);
	fn.vkDestroyInstance(inst, NULL);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkEnumerateInstanceVersion failed (%d)", (int)r);

	char buf[32];
	snprintf(buf, sizeof buf, "%u.%u.%u",
	         (unsigned)VK_API_VERSION_MAJOR(ver), (unsigned)VK_API_VERSION_MINOR(ver), (unsigned)VK_API_VERSION_PATCH(ver));
	return fb_str(S, buf);
}

/* vk.instance_layers() -> List<String>: names of the Vulkan instance layers
 * available on this system (validation layers, overlay layers, etc. -- e.g.
 * "VK_LAYER_KHRONOS_validation" if the LunarG SDK/validation layer is
 * installed). No window needed -- same throwaway-instance trick as
 * vk.devices()/vk.api_version() above. */
static fb_Value bi_instance_layers(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	if (!ensure_loader(S)) return fb_nil(S);
	vk_fns_t fn;
	VkInstance inst = make_instance(S, "fbvk-query", 0, &fn);
	if (!inst) return fb_nil(S);

	uint32_t count = 0;
	fn.vkEnumerateInstanceLayerProperties(&count, NULL);
	VkLayerProperties *layers = malloc(sizeof(*layers) * (count ? count : 1));
	VkResult r = fn.vkEnumerateInstanceLayerProperties(&count, layers);
	fn.vkDestroyInstance(inst, NULL);
	if (r != VK_SUCCESS) { free(layers); return fb_error(S, "vk: vkEnumerateInstanceLayerProperties failed (%d)", (int)r); }

	fb_Value list = fb_list(S);
	for (uint32_t i = 0; i < count; i++)
		fb_push(S, list, fb_str(S, layers[i].layerName));
	free(layers);
	return list;
}

/* ---- app construction ---------------------------------------------------- */

static uint32_t find_memory_type(vkapp_t *app, uint32_t type_bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	app->fn.vkGetPhysicalDeviceMemoryProperties(app->phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

static int create_swapchain(fb_State *S, vkapp_t *app, VkSwapchainKHR old) {
	VkSurfaceCapabilitiesKHR caps;
	app->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app->phys, app->surface, &caps);

	uint32_t fmt_count = 0;
	app->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(app->phys, app->surface, &fmt_count, NULL);
	VkSurfaceFormatKHR *fmts = malloc(sizeof(*fmts) * fmt_count);
	app->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(app->phys, app->surface, &fmt_count, fmts);
	VkSurfaceFormatKHR chosen = fmts[0];
	for (uint32_t i = 0; i < fmt_count; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
	free(fmts);

	int fw, fh;
	g_glfw.GetFramebufferSize(app->win, &fw, &fh);
	VkExtent2D extent;
	if (caps.currentExtent.width != UINT32_MAX) {
		extent = caps.currentExtent;
	} else {
		extent.width  = (uint32_t)fw < caps.minImageExtent.width  ? caps.minImageExtent.width  :
		                ((uint32_t)fw > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : (uint32_t)fw);
		extent.height = (uint32_t)fh < caps.minImageExtent.height ? caps.minImageExtent.height :
		                ((uint32_t)fh > caps.maxImageExtent.height ? caps.maxImageExtent.height : (uint32_t)fh);
	}
	if (extent.width == 0 || extent.height == 0) return 0; /* minimized */

	uint32_t want_images = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && want_images > caps.maxImageCount) want_images = caps.maxImageCount;

	VkSwapchainCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = app->surface,
		.minImageCount = want_images,
		.imageFormat = chosen.format,
		.imageColorSpace = chosen.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, /* always available; simplest vsync'd choice */
		.clipped = VK_TRUE,
		.oldSwapchain = old,
	};
	VkSwapchainKHR sc;
	if (app->fn.vkCreateSwapchainKHR(app->dev, &sci, NULL, &sc) != VK_SUCCESS) {
		fb_error(S, "vk: vkCreateSwapchainKHR failed");
		return 0;
	}
	if (old) app->fn.vkDestroySwapchainKHR(app->dev, old, NULL);

	app->swapchain = sc;
	app->swap_format = chosen.format;
	app->extent = extent;
	app->width = (int)extent.width;
	app->height = (int)extent.height;

	app->fn.vkGetSwapchainImagesKHR(app->dev, sc, &app->image_count, NULL);
	app->images = realloc(app->images, sizeof(*app->images) * app->image_count);
	app->fn.vkGetSwapchainImagesKHR(app->dev, sc, &app->image_count, app->images);

	app->views = realloc(app->views, sizeof(*app->views) * app->image_count);
	for (uint32_t i = 0; i < app->image_count; i++) {
		VkImageViewCreateInfo vi = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = app->images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = app->swap_format,
			.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		app->fn.vkCreateImageView(app->dev, &vi, NULL, &app->views[i]);
	}

	app->fbs = realloc(app->fbs, sizeof(*app->fbs) * app->image_count);
	for (uint32_t i = 0; i < app->image_count; i++) {
		VkFramebufferCreateInfo fi = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = app->renderpass,
			.attachmentCount = 1,
			.pAttachments = &app->views[i],
			.width = app->extent.width,
			.height = app->extent.height,
			.layers = 1,
		};
		app->fn.vkCreateFramebuffer(app->dev, &fi, NULL, &app->fbs[i]);
	}

	app->render_done = realloc(app->render_done, sizeof(*app->render_done) * app->image_count);
	for (uint32_t i = 0; i < app->image_count; i++) {
		VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		app->fn.vkCreateSemaphore(app->dev, &si, NULL, &app->render_done[i]);
	}
	return 1;
}

static void destroy_swapchain_only(vkapp_t *app) {
	for (uint32_t i = 0; i < app->image_count; i++) {
		if (app->fbs) app->fn.vkDestroyFramebuffer(app->dev, app->fbs[i], NULL);
		if (app->views) app->fn.vkDestroyImageView(app->dev, app->views[i], NULL);
		if (app->render_done) app->fn.vkDestroySemaphore(app->dev, app->render_done[i], NULL);
	}
}

/* vk.app(title, w, h[, opts]) -> App. a[0] is `vk` itself (self); real args
 * start at a[1], same self-excluded convention as fbmath's methods. */
static fb_Value bi_app(fb_State *S, fb_Value *a, int n) {
	if (!ensure_loader(S)) return fb_nil(S);
	const char *title = P(a[1]);
	int w = (int)fb_toint(a[2]);
	int h = (int)fb_toint(a[3]);
	long device_index = -1;
	if (n >= 5 && fb_kind(a[4]) == FB_MAP) {
		fb_Value di = fb_field(a[4], "device_index");
		if (fb_kind(di) == FB_INT) device_index = fb_toint(di);
	}

	if (!g_glfw_init_done) {
		if (!g_glfw.Init()) return fb_error(S, "vk: glfwInit failed (no display?)");
		g_glfw_init_done = 1;
	}
	if (!g_glfw.VulkanSupported()) return fb_error(S, "vk: glfwVulkanSupported() says no");

	g_glfw.WindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow *win = g_glfw.CreateWindow(w, h, title, NULL, NULL);
	if (!win) return fb_error(S, "vk: glfwCreateWindow failed");

	vkapp_t *app = calloc(1, sizeof *app);
	app->win = win;
	app->instance = make_instance(S, title, 1, &app->fn);
	if (!app->instance) { free(app); return fb_nil(S); }

	if (g_glfw.CreateWindowSurface(app->instance, win, NULL, &app->surface) != VK_SUCCESS)
		return fb_error(S, "vk: glfwCreateWindowSurface failed");

	uint32_t pd_count = 0;
	app->fn.vkEnumeratePhysicalDevices(app->instance, &pd_count, NULL);
	if (pd_count == 0) return fb_error(S, "vk: no Vulkan-capable physical devices");
	VkPhysicalDevice *pds = malloc(sizeof(*pds) * pd_count);
	app->fn.vkEnumeratePhysicalDevices(app->instance, &pd_count, pds);

	VkPhysicalDevice picked = VK_NULL_HANDLE;
	uint32_t picked_qfam = UINT32_MAX;
	for (uint32_t pass = 0; pass < 2 && picked == VK_NULL_HANDLE; pass++) {
		for (uint32_t i = 0; i < pd_count; i++) {
			if (device_index >= 0 && (long)i != device_index) continue;
			VkPhysicalDeviceProperties props;
			app->fn.vkGetPhysicalDeviceProperties(pds[i], &props);
			if (pass == 0 && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;

			uint32_t qf_count = 0;
			app->fn.vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qf_count, NULL);
			VkQueueFamilyProperties *qfs = malloc(sizeof(*qfs) * qf_count);
			app->fn.vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qf_count, qfs);
			for (uint32_t f = 0; f < qf_count; f++) {
				VkBool32 present = VK_FALSE;
				app->fn.vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], f, app->surface, &present);
				if ((qfs[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
					picked = pds[i]; picked_qfam = f; break;
				}
			}
			free(qfs);
			if (picked) break;
		}
	}
	free(pds);
	if (!picked) return fb_error(S, "vk: no device with a graphics+present queue family");
	app->phys = picked;
	app->qfam = picked_qfam;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = app->qfam, .queueCount = 1, .pQueuePriorities = &prio,
	};
	static const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts,
	};
	if (app->fn.vkCreateDevice(app->phys, &dci, NULL, &app->dev) != VK_SUCCESS)
		return fb_error(S, "vk: vkCreateDevice failed");
	app->fn.vkGetDeviceQueue(app->dev, app->qfam, 0, &app->queue);

	VkPhysicalDeviceProperties props;
	app->fn.vkGetPhysicalDeviceProperties(app->phys, &props);

	/* render pass: single color attachment, clear-then-present. Built before
	 * the swapchain since framebuffers (made per swapchain image) need it. */
	uint32_t fmt_count = 0;
	app->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(app->phys, app->surface, &fmt_count, NULL);
	VkSurfaceFormatKHR *fmts = malloc(sizeof(*fmts) * fmt_count);
	app->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(app->phys, app->surface, &fmt_count, fmts);
	VkFormat rp_format = fmts[0].format;
	for (uint32_t i = 0; i < fmt_count; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { rp_format = fmts[i].format; break; }
	free(fmts);

	VkAttachmentDescription color_att = {
		.format = rp_format, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &color_ref,
	};
	VkSubpassDependency dep = {
		.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	VkRenderPassCreateInfo rpi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &color_att,
		.subpassCount = 1, .pSubpasses = &subpass,
		.dependencyCount = 1, .pDependencies = &dep,
	};
	if (app->fn.vkCreateRenderPass(app->dev, &rpi, NULL, &app->renderpass) != VK_SUCCESS)
		return fb_error(S, "vk: vkCreateRenderPass failed");

	if (!create_swapchain(S, app, VK_NULL_HANDLE))
		return fb_error(S, "vk: initial swapchain creation failed");

	VkCommandPoolCreateInfo cpi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = app->qfam,
	};
	app->fn.vkCreateCommandPool(app->dev, &cpi, NULL, &app->cmdpool);

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = app->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT,
	};
	app->fn.vkAllocateCommandBuffers(app->dev, &cbai, app->cmdbufs);

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
		app->fn.vkCreateSemaphore(app->dev, &si, NULL, &app->img_avail[i]);
		app->fn.vkCreateFence(app->dev, &fi, NULL, &app->in_flight[i]);
	}

	fb_Value obj = fb_object(S, APP_CLS, app);
	fb_set(S, obj, "title", fb_str(S, title));
	fb_set(S, obj, "width", fb_int(S, w));
	fb_set(S, obj, "height", fb_int(S, h));
	fb_set(S, obj, "device_name", fb_str(S, props.deviceName));
	return obj;
}

/* ---- App methods ---------------------------------------------------------- */

static fb_Value m_should_close(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	return fb_bool(S, g_glfw.WindowShouldClose(app->win));
}

static fb_Value m_poll(fb_State *S, fb_Value *a, int n) {
	(void)S; (void)n;
	g_glfw.PollEvents();
	return a[0];
}

/* .wait_idle() -> Bool. block until the graphics queue has drained --
 * useful before touching/destroying a resource still in flight. */
static fb_Value m_wait_idle(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	VkResult r = app->fn.vkQueueWaitIdle(app->queue);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkQueueWaitIdle failed (%d)", (int)r);
	return fb_bool(S, 1);
}

/* .reset_command_pool() -> Bool. bulk-reset every command buffer allocated
 * from this app's command pool back to the initial state in one call,
 * instead of resetting each command buffer individually -- useful when
 * abandoning frames mid-recording and starting clean. */
static fb_Value m_reset_command_pool(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	VkResult r = app->fn.vkResetCommandPool(app->dev, app->cmdpool, 0);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkResetCommandPool failed (%d)", (int)r);
	return fb_bool(S, 1);
}

/* .frame_ready() -> Bool. non-blocking poll of the current frame slot's
 * in-flight fence -- true once the GPU has finished the last submission that
 * used this slot (the same fence begin_frame would otherwise block on),
 * false if it's still executing. Lets a script pace itself (skip other work
 * while waiting) instead of only having the blocking begin_frame/wait_idle
 * choice. */
static fb_Value m_frame_ready(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	VkFence fence = app->in_flight[app->frame];
	VkResult r = app->fn.vkGetFenceStatus(app->dev, fence);
	if (r == VK_SUCCESS) return fb_bool(S, 1);
	if (r == VK_NOT_READY) return fb_bool(S, 0);
	return fb_error(S, "vk: vkGetFenceStatus failed (%d)", (int)r);
}

/* .begin_frame(r,g,b[,a]) -> Bool. false = swapchain not ready this tick
 * (minimized / just resized) -- caller should just poll and try again. */
static fb_Value m_begin_frame(fb_State *S, fb_Value *a, int n) {
	vkapp_t *app = fb_object_data(a[0]);
	if (app->frame_active) return fb_error(S, "vk: begin_frame called before a previous end_frame");

	VkFence fence = app->in_flight[app->frame];
	app->fn.vkWaitForFences(app->dev, 1, &fence, VK_TRUE, UINT64_MAX);

	uint32_t idx = 0;
	VkResult r = app->fn.vkAcquireNextImageKHR(app->dev, app->swapchain, UINT64_MAX,
	                                            app->img_avail[app->frame], VK_NULL_HANDLE, &idx);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) {
		destroy_swapchain_only(app);
		if (!create_swapchain(S, app, app->swapchain)) return fb_bool(S, 0);
		return fb_bool(S, 0);
	}
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		return fb_error(S, "vk: vkAcquireNextImageKHR failed (%d)", (int)r);

	app->fn.vkResetFences(app->dev, 1, &fence);
	app->cur_image = idx;

	VkCommandBuffer cb = app->cmdbufs[app->frame];
	app->fn.vkResetCommandBuffer(cb, 0);
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	app->fn.vkBeginCommandBuffer(cb, &bi);

	double rr = fb_tofloat(a[1]), gg = fb_tofloat(a[2]), bb = fb_tofloat(a[3]);
	double aa = n >= 5 ? fb_tofloat(a[4]) : 1.0;
	VkClearValue clear = { .color = { { (float)rr, (float)gg, (float)bb, (float)aa } } };
	VkRenderPassBeginInfo rpbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = app->renderpass, .framebuffer = app->fbs[idx],
		.renderArea = { { 0, 0 }, app->extent },
		.clearValueCount = 1, .pClearValues = &clear,
	};
	app->fn.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = { 0, 0, (float)app->extent.width, (float)app->extent.height, 0.0f, 1.0f };
	VkRect2D sc = { { 0, 0 }, app->extent };
	app->fn.vkCmdSetViewport(cb, 0, 1, &vp);
	app->fn.vkCmdSetScissor(cb, 0, 1, &sc);

	app->frame_active = 1;
	return fb_bool(S, 1);
}

static fb_Value m_draw(fb_State *S, fb_Value *a, int n) {
	vkapp_t *app = fb_object_data(a[0]);
	if (!app->frame_active) return fb_error(S, "vk: draw called outside begin_frame/end_frame");
	vkpipe_t *pipe = fb_object_data(a[1]);
	vkbuf_t *vbuf = fb_object_data(a[2]);
	long count = n >= 4 ? fb_toint(a[3]) : vbuf->count;

	VkCommandBuffer cb = app->cmdbufs[app->frame];
	app->fn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
	VkDeviceSize off = 0;
	app->fn.vkCmdBindVertexBuffers(cb, 0, 1, &vbuf->buf, &off);
	app->fn.vkCmdDraw(cb, (uint32_t)count, 1, 0, 0);
	return a[0];
}

static fb_Value m_draw_indexed(fb_State *S, fb_Value *a, int n) {
	vkapp_t *app = fb_object_data(a[0]);
	if (!app->frame_active) return fb_error(S, "vk: draw_indexed called outside begin_frame/end_frame");
	vkpipe_t *pipe = fb_object_data(a[1]);
	vkbuf_t *vbuf = fb_object_data(a[2]);
	vkbuf_t *ibuf = fb_object_data(a[3]);
	long count = n >= 5 ? fb_toint(a[4]) : ibuf->count;

	VkCommandBuffer cb = app->cmdbufs[app->frame];
	app->fn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
	VkDeviceSize off = 0;
	app->fn.vkCmdBindVertexBuffers(cb, 0, 1, &vbuf->buf, &off);
	app->fn.vkCmdBindIndexBuffer(cb, ibuf->buf, 0, VK_INDEX_TYPE_UINT32);
	app->fn.vkCmdDrawIndexed(cb, (uint32_t)count, 1, 0, 0, 0);
	return a[0];
}

static fb_Value m_end_frame(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	if (!app->frame_active) return fb_error(S, "vk: end_frame without a matching begin_frame");
	VkCommandBuffer cb = app->cmdbufs[app->frame];
	app->fn.vkCmdEndRenderPass(cb);
	app->fn.vkEndCommandBuffer(cb);

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1, .pWaitSemaphores = &app->img_avail[app->frame], .pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1, .pCommandBuffers = &cb,
		.signalSemaphoreCount = 1, .pSignalSemaphores = &app->render_done[app->cur_image],
	};
	app->fn.vkQueueSubmit(app->queue, 1, &si, app->in_flight[app->frame]);

	VkPresentInfoKHR pi = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1, .pWaitSemaphores = &app->render_done[app->cur_image],
		.swapchainCount = 1, .pSwapchains = &app->swapchain, .pImageIndices = &app->cur_image,
	};
	VkResult r = app->fn.vkQueuePresentKHR(app->queue, &pi);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
		destroy_swapchain_only(app);
		create_swapchain(S, app, app->swapchain);
	} else if (r != VK_SUCCESS) {
		fb_error(S, "vk: vkQueuePresentKHR failed (%d)", (int)r);
	}

	app->frame = (app->frame + 1) % MAX_FRAMES_IN_FLIGHT;
	app->frame_active = 0;
	return a[0];
}

/* .buffer(list, usage) -> Buffer. Host-visible + coherent, persistently
 * mapped: simplest correct approach for a scripting wrapper (no staging
 * buffer / device-local upload -- fine for the vertex/index/uniform data
 * sizes a build script realistically hands it). */
static fb_Value bi_buffer(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	fb_Value list = a[1];
	const char *usage_s = P(a[2]);

	int is_index = strcmp(usage_s, "index") == 0;
	VkBufferUsageFlags usage;
	if (strcmp(usage_s, "vertex") == 0) usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	else if (is_index) usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	else if (strcmp(usage_s, "uniform") == 0) usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	else return fb_error(S, "vk: buffer usage must be \"vertex\", \"index\", or \"uniform\", got \"%s\"", usage_s);
	usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT; /* lets .fill() (vkCmdFillBuffer) target it */
	usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT; /* lets .copy_to() (vkCmdCopyBuffer) read it */

	int len = fb_len(list);
	size_t elem_size = is_index ? sizeof(uint32_t) : sizeof(float);
	VkDeviceSize size = (VkDeviceSize)len * elem_size;
	if (size == 0) size = elem_size;

	vkbuf_t *b = calloc(1, sizeof *b);
	b->app = app; b->count = len; b->is_index = is_index; b->size = size;

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (app->fn.vkCreateBuffer(app->dev, &bci, NULL, &b->buf) != VK_SUCCESS)
		return fb_error(S, "vk: vkCreateBuffer failed");

	VkMemoryRequirements req;
	app->fn.vkGetBufferMemoryRequirements(app->dev, b->buf, &req);
	uint32_t mt = find_memory_type(app, req.memoryTypeBits,
	                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mt == UINT32_MAX) return fb_error(S, "vk: no host-visible+coherent memory type for this buffer");

	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt };
	if (app->fn.vkAllocateMemory(app->dev, &mai, NULL, &b->mem) != VK_SUCCESS)
		return fb_error(S, "vk: vkAllocateMemory failed");
	app->fn.vkBindBufferMemory(app->dev, b->buf, b->mem, 0);
	app->fn.vkMapMemory(app->dev, b->mem, 0, size, 0, &b->mapped);

	if (is_index) {
		uint32_t *dst = b->mapped;
		for (int i = 0; i < len; i++) dst[i] = (uint32_t)fb_toint(fb_at(list, i));
	} else {
		float *dst = b->mapped;
		for (int i = 0; i < len; i++) dst[i] = (float)fb_tofloat(fb_at(list, i));
	}

	fb_Value obj = fb_object(S, BUFFER_CLS, b);
	fb_set(S, obj, "size", fb_int(S, (long)size));
	fb_set(S, obj, "count", fb_int(S, len));
	fb_set(S, obj, "usage", fb_str(S, usage_s));
	return obj;
}

static fb_Value m_buf_update(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkbuf_t *b = fb_object_data(a[0]);
	fb_Value list = a[1];
	int len = fb_len(list);
	if ((size_t)len * (b->is_index ? sizeof(uint32_t) : sizeof(float)) > b->size)
		return fb_error(S, "vk: update() data longer than buffer's original size");
	if (b->is_index) {
		uint32_t *dst = b->mapped;
		for (int i = 0; i < len; i++) dst[i] = (uint32_t)fb_toint(fb_at(list, i));
	} else {
		float *dst = b->mapped;
		for (int i = 0; i < len; i++) dst[i] = (float)fb_tofloat(fb_at(list, i));
	}
	b->count = len;
	return a[0];
}

/* .fill(value) -> self. GPU-side fill of the whole buffer with a repeating
 * 32-bit word (vkCmdFillBuffer) -- unlike .update(), no host memory touched
 * by the script; recorded on a one-shot command buffer allocated from the
 * app's own command pool, submitted, and waited on synchronously, so it
 * works even outside begin_frame/end_frame. size is always a multiple of 4
 * (every buffer here is built from 4-byte Float/Int elements), satisfying
 * vkCmdFillBuffer's alignment requirement. */
static fb_Value m_buf_fill(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkbuf_t *b = fb_object_data(a[0]);
	vkapp_t *app = b->app;
	uint32_t value = (uint32_t)fb_toint(a[1]);

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = app->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb;
	VkResult r = app->fn.vkAllocateCommandBuffers(app->dev, &cbai, &cb);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkAllocateCommandBuffers failed (%d)", (int)r);

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	app->fn.vkBeginCommandBuffer(cb, &bi);
	app->fn.vkCmdFillBuffer(cb, b->buf, 0, VK_WHOLE_SIZE, value);
	app->fn.vkEndCommandBuffer(cb);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb,
	};
	r = app->fn.vkQueueSubmit(app->queue, 1, &si, VK_NULL_HANDLE);
	if (r == VK_SUCCESS) r = app->fn.vkQueueWaitIdle(app->queue);
	app->fn.vkFreeCommandBuffers(app->dev, app->cmdpool, 1, &cb);
	if (r != VK_SUCCESS) return fb_error(S, "vk: fill submit/wait failed (%d)", (int)r);
	return a[0];
}

/* .copy_to(dst) -> self. GPU-side copy of this buffer's data into `dst`
 * (vkCmdCopyBuffer), min(this.size, dst.size) bytes starting at offset 0 in
 * both; recorded/submitted/waited the same one-shot way as .fill(), so also
 * works outside begin_frame/end_frame. Every buffer built by .buffer() sets
 * both TRANSFER_SRC_BIT and TRANSFER_DST_BIT, so any Buffer can be either
 * side of a copy_to(). */
static fb_Value m_buf_copy_to(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkbuf_t *src = fb_object_data(a[0]);
	vkbuf_t *dst = fb_object_data(a[1]);
	vkapp_t *app = src->app;
	VkDeviceSize size = src->size < dst->size ? src->size : dst->size;

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = app->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb;
	VkResult r = app->fn.vkAllocateCommandBuffers(app->dev, &cbai, &cb);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkAllocateCommandBuffers failed (%d)", (int)r);

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	app->fn.vkBeginCommandBuffer(cb, &bi);
	VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = size };
	app->fn.vkCmdCopyBuffer(cb, src->buf, dst->buf, 1, &region);
	app->fn.vkEndCommandBuffer(cb);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb,
	};
	r = app->fn.vkQueueSubmit(app->queue, 1, &si, VK_NULL_HANDLE);
	if (r == VK_SUCCESS) r = app->fn.vkQueueWaitIdle(app->queue);
	app->fn.vkFreeCommandBuffers(app->dev, app->cmdpool, 1, &cb);
	if (r != VK_SUCCESS) return fb_error(S, "vk: copy_to submit/wait failed (%d)", (int)r);
	return a[0];
}

static fb_Value m_buf_destroy(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkbuf_t *b = fb_object_data(a[0]);
	if (b->buf) {
		b->app->fn.vkUnmapMemory(b->app->dev, b->mem);
		b->app->fn.vkDestroyBuffer(b->app->dev, b->buf, NULL);
		b->app->fn.vkFreeMemory(b->app->dev, b->mem, NULL);
		b->buf = VK_NULL_HANDLE;
	}
	return fb_bool(S, 1);
	(void)S;
}

/* .shader(src_or_path, stage[, entry]) -> Shader. A ".spv" path is loaded
 * as raw SPIR-V bytes; anything else is treated as GLSL source (inline
 * string or a source file path) and compiled on the fly via the `glslc`
 * CLI (must be on PATH). */
static VkShaderStageFlagBits stage_from_str(fb_State *S, const char *s, const char **glslc_stage) {
	if (strcmp(s, "vertex") == 0)   { *glslc_stage = "vertex";   return VK_SHADER_STAGE_VERTEX_BIT; }
	if (strcmp(s, "fragment") == 0) { *glslc_stage = "fragment"; return VK_SHADER_STAGE_FRAGMENT_BIT; }
	if (strcmp(s, "compute") == 0)  { *glslc_stage = "compute";  return VK_SHADER_STAGE_COMPUTE_BIT; }
	fb_error(S, "vk: shader stage must be \"vertex\", \"fragment\", or \"compute\", got \"%s\"", s);
	return 0;
}

static char *read_whole_file(const char *path, size_t *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(sz > 0 ? (size_t)sz : 1);
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	*out_len = got;
	return buf;
}

static fb_Value bi_shader(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	const char *src = P(a[1]);
	const char *stage_s = P(a[2]);
	const char *glslc_stage;
	VkShaderStageFlagBits stage = stage_from_str(S, stage_s, &glslc_stage);
	if (!stage) return fb_nil(S);

	uint32_t *code = NULL;
	size_t code_len = 0;
	size_t src_len = strlen(src);
	int looks_like_path = src_len < 4096 && strchr(src, '\n') == NULL && access(src, F_OK) == 0;

	if (looks_like_path && src_len > 4 && strcmp(src + src_len - 4, ".spv") == 0) {
		size_t n_bytes;
		char *bytes = read_whole_file(src, &n_bytes);
		if (!bytes) return fb_error(S, "vk: cannot read '%s'", src);
		code = (uint32_t *)bytes;
		code_len = n_bytes;
	} else {
		char in_path[] = "/tmp/fbvk-shader-XXXXXX";
		int fd = mkstemp(in_path);
		if (fd < 0) return fb_error(S, "vk: mkstemp failed");
		if (looks_like_path) {
			size_t n_bytes;
			char *bytes = read_whole_file(src, &n_bytes);
			if (!bytes) { close(fd); unlink(in_path); return fb_error(S, "vk: cannot read '%s'", src); }
			write(fd, bytes, n_bytes);
			free(bytes);
		} else {
			write(fd, src, src_len);
		}
		close(fd);

		char out_path[64];
		snprintf(out_path, sizeof out_path, "%s.spv", in_path);
		char cmd[512];
		snprintf(cmd, sizeof cmd, "glslc -fshader-stage=%s '%s' -o '%s' 2>&1",
		         glslc_stage, in_path, out_path);
		FILE *p = popen(cmd, "r");
		char errbuf[2048] = {0};
		if (p) { size_t got = fread(errbuf, 1, sizeof errbuf - 1, p); (void)got; pclose(p); }
		unlink(in_path);

		size_t n_bytes = 0;
		char *bytes = read_whole_file(out_path, &n_bytes);
		unlink(out_path);
		if (!bytes) return fb_error(S, "vk: glslc failed compiling %s shader: %s", glslc_stage, errbuf);
		code = (uint32_t *)bytes;
		code_len = n_bytes;
	}

	VkShaderModuleCreateInfo smi = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_len, .pCode = code,
	};
	VkShaderModule mod;
	VkResult r = app->fn.vkCreateShaderModule(app->dev, &smi, NULL, &mod);
	free(code);
	if (r != VK_SUCCESS) return fb_error(S, "vk: vkCreateShaderModule failed (invalid SPIR-V?)");

	vkshader_t *sh = calloc(1, sizeof *sh);
	sh->app = app; sh->mod = mod; sh->stage = stage;
	fb_Value obj = fb_object(S, SHADER_CLS, sh);
	fb_set(S, obj, "stage", fb_str(S, stage_s));
	return obj;
}

static fb_Value m_shader_destroy(fb_State *S, fb_Value *a, int n) {
	(void)S; (void)n;
	vkshader_t *sh = fb_object_data(a[0]);
	if (sh->mod) { sh->app->fn.vkDestroyShaderModule(sh->app->dev, sh->mod, NULL); sh->mod = VK_NULL_HANDLE; }
	return fb_bool(S, 1);
}

/* .vertex_format_supported(comps) -> Bool. `comps` is a per-attribute float
 * component count (1..4), exactly what pipeline()'s `layout` list elements
 * are -- this checks whether the VkFormat that comps maps to there
 * (R32_SFLOAT..R32G32B32A32_SFLOAT) is actually usable as a vertex buffer
 * format on this device, so a script can sanity-check a layout before
 * building a pipeline with it instead of only finding out from a validation
 * layer message. Every mainstream GPU supports all four, but the query costs
 * nothing and this is what real Vulkan code does before trusting a format. */
static fb_Value m_vertex_format_supported(fb_State *S, fb_Value *a, int n) {
	(void)n;
	vkapp_t *app = fb_object_data(a[0]);
	int comps = (int)fb_toint(a[1]);
	VkFormat fmt = comps == 1 ? VK_FORMAT_R32_SFLOAT :
	               comps == 2 ? VK_FORMAT_R32G32_SFLOAT :
	               comps == 3 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
	VkFormatProperties props;
	app->fn.vkGetPhysicalDeviceFormatProperties(app->phys, fmt, &props);
	return fb_bool(S, (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0);
}

/* .pipeline(vert, frag, layout[, opts]) -> Pipeline. `layout` is a list of
 * per-attribute float component counts, e.g. List<3,3> = a tightly packed
 * vec3 position + vec3 color per vertex (one binding, one buffer). */
static fb_Value bi_pipeline(fb_State *S, fb_Value *a, int n) {
	vkapp_t *app = fb_object_data(a[0]);
	vkshader_t *vert = fb_object_data(a[1]);
	vkshader_t *frag = fb_object_data(a[2]);
	fb_Value layout = a[3];

	const char *cull_s = "none";
	int wireframe = 0;
	VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	if (n >= 5 && fb_kind(a[4]) == FB_MAP) {
		fb_Value c = fb_field(a[4], "cull");
		if (fb_kind(c) == FB_STR) cull_s = P(c);
		fb_Value wf = fb_field(a[4], "wireframe");
		if (fb_kind(wf) == FB_BOOL) wireframe = (int)fb_toint(wf);
		fb_Value t = fb_field(a[4], "topology");
		if (fb_kind(t) == FB_STR) {
			const char *ts = P(t);
			if (strcmp(ts, "lines") == 0) topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			else if (strcmp(ts, "points") == 0) topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		}
	}
	VkCullModeFlags cull = VK_CULL_MODE_NONE;
	if (strcmp(cull_s, "back") == 0) cull = VK_CULL_MODE_BACK_BIT;
	else if (strcmp(cull_s, "front") == 0) cull = VK_CULL_MODE_FRONT_BIT;

	int nattrs = fb_len(layout);
	VkVertexInputAttributeDescription *attrs = malloc(sizeof(*attrs) * (nattrs ? nattrs : 1));
	uint32_t offset = 0;
	for (int i = 0; i < nattrs; i++) {
		int comps = (int)fb_toint(fb_at(layout, i));
		VkFormat fmt = comps == 1 ? VK_FORMAT_R32_SFLOAT :
		               comps == 2 ? VK_FORMAT_R32G32_SFLOAT :
		               comps == 3 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
		attrs[i] = (VkVertexInputAttributeDescription){ .location = (uint32_t)i, .binding = 0, .format = fmt, .offset = offset };
		offset += (uint32_t)comps * sizeof(float);
	}
	VkVertexInputBindingDescription binding = { .binding = 0, .stride = offset, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
	VkPipelineVertexInputStateCreateInfo vis = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = nattrs ? 1 : 0, .pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = (uint32_t)nattrs, .pVertexAttributeDescriptions = attrs,
	};

	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo,
	};
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
		.cullMode = cull, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState cba = {
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba,
	};
	static const VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dyn = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn_states,
	};
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = vert->stage, .module = vert->mod, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = frag->stage, .module = frag->mod, .pName = "main" },
	};

	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout_h;
	if (app->fn.vkCreatePipelineLayout(app->dev, &pli, NULL, &layout_h) != VK_SUCCESS) {
		free(attrs);
		return fb_error(S, "vk: vkCreatePipelineLayout failed");
	}

	VkGraphicsPipelineCreateInfo gpi = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages,
		.pVertexInputState = &vis, .pInputAssemblyState = &ia, .pViewportState = &vp,
		.pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dyn,
		.layout = layout_h, .renderPass = app->renderpass, .subpass = 0,
	};
	VkPipeline pipeline;
	VkResult r = app->fn.vkCreateGraphicsPipelines(app->dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipeline);
	free(attrs);
	if (r != VK_SUCCESS) {
		app->fn.vkDestroyPipelineLayout(app->dev, layout_h, NULL);
		return fb_error(S, "vk: vkCreateGraphicsPipelines failed");
	}

	vkpipe_t *p = calloc(1, sizeof *p);
	p->app = app; p->pipeline = pipeline; p->layout = layout_h;
	return fb_object(S, PIPELINE_CLS, p);
}

static fb_Value m_pipeline_destroy(fb_State *S, fb_Value *a, int n) {
	(void)S; (void)n;
	vkpipe_t *p = fb_object_data(a[0]);
	if (p->pipeline) {
		p->app->fn.vkDestroyPipeline(p->app->dev, p->pipeline, NULL);
		p->app->fn.vkDestroyPipelineLayout(p->app->dev, p->layout, NULL);
		p->pipeline = VK_NULL_HANDLE;
	}
	return fb_bool(S, 1);
}

static fb_Value m_app_destroy(fb_State *S, fb_Value *a, int n) {
	(void)S; (void)n;
	vkapp_t *app = fb_object_data(a[0]);
	if (!app->dev) return fb_bool(S, 1);
	app->fn.vkDeviceWaitIdle(app->dev);

	destroy_swapchain_only(app);
	app->fn.vkDestroySwapchainKHR(app->dev, app->swapchain, NULL);
	free(app->images); free(app->views); free(app->fbs); free(app->render_done);

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		app->fn.vkDestroySemaphore(app->dev, app->img_avail[i], NULL);
		app->fn.vkDestroyFence(app->dev, app->in_flight[i], NULL);
	}
	app->fn.vkDestroyCommandPool(app->dev, app->cmdpool, NULL);
	app->fn.vkDestroyRenderPass(app->dev, app->renderpass, NULL);
	app->fn.vkDestroyDevice(app->dev, NULL);
	app->fn.vkDestroySurfaceKHR(app->instance, app->surface, NULL);
	app->fn.vkDestroyInstance(app->instance, NULL);
	g_glfw.DestroyWindow(app->win);
	app->dev = VK_NULL_HANDLE;
	return fb_bool(S, 1);
}

/* vk is a namespace object, not a resource -- calling it directly is a
 * mistake (same convention as fbmath's `math` / fbregex's `regex`). */
static fb_Value m_noncallable(fb_State *S, fb_Value *a, int n) {
	(void)a; (void)n;
	return fb_error(S, "vk is a namespace object; call e.g. vk.app(...), not vk(...)");
}

int fbuild_abi(void) { return FBUILD_ABI; }

int fbuild_open(fb_State *S) {
	VK_CLS       = fb_class(S, "vk"); /* namespace class -- name matches the
	                                    * global-bind identifier below so
	                                    * fb_method's shadow entries land as
	                                    * "vk.method" (see fbmath.c's `math`) */
	DEVICE_CLS   = fb_class(S, "Device");
	APP_CLS      = fb_class(S, "App");
	BUFFER_CLS   = fb_class(S, "Buffer");
	SHADER_CLS   = fb_class(S, "Shader");
	PIPELINE_CLS = fb_class(S, "Pipeline");

	static const fb_Ret NUM4[] = { FB_RET_NUM, FB_RET_NUM, FB_RET_NUM, FB_RET_NUM };
	fb_method(S, APP_CLS, "should_close", m_should_close,
	          &(fb_Reg){ "should_close", m_should_close, 0, 0, FB_RET_BOOL, 0,
	                     "true once the window's close button/Alt+F4 was hit", NULL, 0, NULL, NULL, NULL });
	fb_method(S, APP_CLS, "poll", m_poll,
	          &(fb_Reg){ "poll", m_poll, 0, 0, FB_RET_ANY, 0, "pump window/input events; chainable",
	                     NULL, 0, "App", NULL, NULL });
	fb_method(S, APP_CLS, "wait_idle", m_wait_idle,
	          &(fb_Reg){ "wait_idle", m_wait_idle, 0, 0, FB_RET_BOOL, 0,
	                     "block until the graphics queue is idle", NULL, 0, NULL, NULL, NULL });
	fb_method(S, APP_CLS, "reset_command_pool", m_reset_command_pool,
	          &(fb_Reg){ "reset_command_pool", m_reset_command_pool, 0, 0, FB_RET_BOOL, 0,
	                     "reset every command buffer allocated from this app's command pool at once",
	                     NULL, 0, NULL, NULL, NULL });
	fb_method(S, APP_CLS, "frame_ready", m_frame_ready,
	          &(fb_Reg){ "frame_ready", m_frame_ready, 0, 0, FB_RET_BOOL, 0,
	                     "non-blocking check: has the GPU finished the current frame slot's last submission",
	                     NULL, 0, NULL, NULL, NULL });
	fb_method(S, APP_CLS, "begin_frame", m_begin_frame,
	          &(fb_Reg){ "begin_frame", m_begin_frame, 3, 4, FB_RET_BOOL, 0,
	                     "acquire swapchain image + begin render pass with clear color r,g,b[,a] (0..1); "
	                     "false = skip this tick (resized/minimized), just poll() and retry",
	                     NUM4, 4, NULL, NULL, NULL });
	static const fb_Ret draw_types[]     = { FB_RET_ANY, FB_RET_ANY, FB_RET_INT };
	fb_method(S, APP_CLS, "draw", m_draw,
	          &(fb_Reg){ "draw", m_draw, 2, 3, FB_RET_ANY, 0,
	                     "bind pipeline + vertex buffer, vkCmdDraw; chainable. Must be between begin_frame/end_frame",
	                     draw_types, 3, "App", NULL, NULL });
	fb_reg_param_class("App.draw", 0, "Pipeline");
	fb_reg_param_class("App.draw", 1, "Buffer");
	static const fb_Ret draw_idx_types[] = { FB_RET_ANY, FB_RET_ANY, FB_RET_ANY, FB_RET_INT };
	fb_method(S, APP_CLS, "draw_indexed", m_draw_indexed,
	          &(fb_Reg){ "draw_indexed", m_draw_indexed, 3, 4, FB_RET_ANY, 0,
	                     "bind pipeline + vertex/index buffers, vkCmdDrawIndexed; chainable",
	                     draw_idx_types, 4, "App", NULL, NULL });
	fb_reg_param_class("App.draw_indexed", 0, "Pipeline");
	fb_reg_param_class("App.draw_indexed", 1, "Buffer");
	fb_reg_param_class("App.draw_indexed", 2, "Buffer");
	fb_method(S, APP_CLS, "end_frame", m_end_frame,
	          &(fb_Reg){ "end_frame", m_end_frame, 0, 0, FB_RET_ANY, 0,
	                     "end render pass, submit, present; chainable", NULL, 0, "App", NULL, NULL });
	static const fb_Ret buf_params[] = { FB_RET_LIST, FB_RET_STR };
	fb_method(S, APP_CLS, "buffer", bi_buffer,
	          &(fb_Reg){ "buffer", bi_buffer, 2, 2, FB_RET_ANY, 0,
	                     "host-visible buffer from a flat List<Float> (or List<Int> for \"index\"); "
	                     "usage: \"vertex\"|\"index\"|\"uniform\"", buf_params, 2, "Buffer", NULL, NULL });
	static const fb_Ret shader_params[] = { FB_RET_STR, FB_RET_STR };
	fb_method(S, APP_CLS, "shader", bi_shader,
	          &(fb_Reg){ "shader", bi_shader, 2, 3, FB_RET_ANY, 0,
	                     "GLSL source/path (compiled via `glslc`) or a .spv path; stage: "
	                     "\"vertex\"|\"fragment\"|\"compute\"", shader_params, 2, "Shader", NULL, NULL });
	static const fb_Ret pipe_types[] = { FB_RET_ANY, FB_RET_ANY, FB_RET_LIST, FB_RET_MAP };
	fb_method(S, APP_CLS, "pipeline", bi_pipeline,
	          &(fb_Reg){ "pipeline", bi_pipeline, 3, 4, FB_RET_ANY, 0,
	                     "graphics pipeline from vert/frag Shaders + a vertex layout, e.g. List<3,3> = "
	                     "vec3+vec3 per vertex; opts map: cull, wireframe, topology",
	                     pipe_types, 4, "Pipeline", NULL, NULL });
	fb_reg_param_class("App.pipeline", 0, "Shader");
	fb_reg_param_class("App.pipeline", 1, "Shader");
	static const fb_Ret vfmt_params[] = { FB_RET_INT };
	fb_method(S, APP_CLS, "vertex_format_supported", m_vertex_format_supported,
	          &(fb_Reg){ "vertex_format_supported", m_vertex_format_supported, 1, 1, FB_RET_BOOL, 0,
	                     "true if this device supports a comps-float (1..4) vertex attribute format, "
	                     "the same VkFormat pipeline()'s layout list would pick for that comps value",
	                     vfmt_params, 1, NULL, NULL, NULL });
	fb_method(S, APP_CLS, "destroy", m_app_destroy,
	          &(fb_Reg){ "destroy", m_app_destroy, 0, 0, FB_RET_BOOL, 0,
	                     "tear down swapchain/device/instance/window; call last", NULL, 0, NULL, NULL, NULL });
	fb_class_field(S, APP_CLS, "title", FB_RET_STR, NULL);
	fb_class_field(S, APP_CLS, "width", FB_RET_INT, NULL);
	fb_class_field(S, APP_CLS, "height", FB_RET_INT, NULL);
	fb_class_field(S, APP_CLS, "device_name", FB_RET_STR, NULL);

	fb_method(S, BUFFER_CLS, "update", m_buf_update,
	          &(fb_Reg){ "update", m_buf_update, 1, 1, FB_RET_ANY, 0,
	                     "re-upload data (must fit the buffer's original size); chainable",
	                     buf_params, 1, "Buffer", NULL, NULL });
	static const fb_Ret fill_params[] = { FB_RET_INT };
	fb_method(S, BUFFER_CLS, "fill", m_buf_fill,
	          &(fb_Reg){ "fill", m_buf_fill, 1, 1, FB_RET_ANY, 0,
	                     "GPU-side fill of the whole buffer with a repeating 32-bit word "
	                     "(vkCmdFillBuffer); chainable, no host memory involved",
	                     fill_params, 1, "Buffer", NULL, NULL });
	static const fb_Ret copy_to_params[] = { FB_RET_ANY };
	fb_method(S, BUFFER_CLS, "copy_to", m_buf_copy_to,
	          &(fb_Reg){ "copy_to", m_buf_copy_to, 1, 1, FB_RET_ANY, 0,
	                     "GPU-side copy of this buffer's data into dst (vkCmdCopyBuffer), "
	                     "min(size, dst.size) bytes; chainable, no host memory involved",
	                     copy_to_params, 1, "Buffer", NULL, NULL });
	fb_reg_param_class("Buffer.copy_to", 0, "Buffer");
	fb_method(S, BUFFER_CLS, "destroy", m_buf_destroy,
	          &(fb_Reg){ "destroy", m_buf_destroy, 0, 0, FB_RET_BOOL, 0, "free GPU memory", NULL, 0, NULL, NULL, NULL });
	fb_class_field(S, BUFFER_CLS, "size", FB_RET_INT, NULL);
	fb_class_field(S, BUFFER_CLS, "count", FB_RET_INT, NULL);
	fb_class_field(S, BUFFER_CLS, "usage", FB_RET_STR, NULL);

	fb_method(S, SHADER_CLS, "destroy", m_shader_destroy,
	          &(fb_Reg){ "destroy", m_shader_destroy, 0, 0, FB_RET_BOOL, 0, "destroy the shader module", NULL, 0, NULL, NULL, NULL });
	fb_class_field(S, SHADER_CLS, "stage", FB_RET_STR, NULL);

	fb_method(S, PIPELINE_CLS, "destroy", m_pipeline_destroy,
	          &(fb_Reg){ "destroy", m_pipeline_destroy, 0, 0, FB_RET_BOOL, 0, "destroy pipeline + its layout", NULL, 0, NULL, NULL, NULL });

	static const fb_Ret app_params[] = { FB_RET_STR, FB_RET_INT, FB_RET_INT, FB_RET_MAP };
	fb_method(S, VK_CLS, "app", bi_app,
	          &(fb_Reg){ "app", bi_app, 3, 4, FB_RET_ANY, 0,
	                     "open a window + Vulkan instance/device/swapchain/render pass, ready to draw",
	                     app_params, 4, "App", NULL, NULL });
	fb_class_field(S, DEVICE_CLS, "features", FB_RET_MAP, NULL);
	fb_method(S, VK_CLS, "devices", bi_devices,
	          &(fb_Reg){ "devices", bi_devices, 0, 0, FB_RET_LIST, 0,
	                     "enumerate Vulkan-capable GPUs (no window needed): name, type, vram, "
	                     "api_version, driver_version, limits, features", NULL, 0, NULL, NULL, NULL });
	fb_method(S, VK_CLS, "api_version", bi_api_version,
	          &(fb_Reg){ "api_version", bi_api_version, 0, 0, FB_RET_STR, 0,
	                     "Vulkan loader/instance API version (\"major.minor.patch\"); no window needed",
	                     NULL, 0, NULL, NULL, NULL });
	fb_method(S, VK_CLS, "instance_layers", bi_instance_layers,
	          &(fb_Reg){ "instance_layers", bi_instance_layers, 0, 0, FB_RET_LIST_STR, 0,
	                     "names of available Vulkan instance layers (validation/overlay/etc); no window needed",
	                     NULL, 0, NULL, NULL, NULL });

	fb_Value obj = fb_object(S, VK_CLS, NULL);
	fb_register(S, &(fb_Reg){ "vk", m_noncallable, 0, -1, FB_RET_ANY, FB_GLOBAL,
	            "vk namespace (fbvk): devices(), app(title,w,h) -> App w/ should_close/poll/"
	            "begin_frame/draw/draw_indexed/end_frame/buffer/shader/pipeline/destroy",
	            NULL, 0, NULL, NULL, NULL });
	fb_global_bind(S, "vk", obj);
	return 0;
}

/* ============================================================================
 * Vulkan registry coverage (generated 2026-07-26, vulkan_core.h header
 * version 275 / Vulkan 1.3, from the local Khronos headers -- i.e. every
 * command and type in vulkan_core.h, the full non-platform-specific core API
 * surface (excludes vulkan_win32.h/xlib.h/etc's platform-only extensions,
 * which this plugin never touches -- window/surface creation goes through
 * GLFW's glfwCreateWindowSurface instead of a raw VK_KHR_*_surface call).
 *
 * Regenerate the raw lists this was built from:
 *   grep -oP '(?<=VKAPI_CALL )vk[A-Za-z0-9_]+(?=\()' vulkan_core.h | sort -u
 *   (types: struct/union/enum `} VkXxx;` + VK_DEFINE_(NON_DISPATCHABLE_)HANDLE
 *    + `typedef VkFlags(64) VkXxx;` + `PFN_vkXxx` + single-line aliases)
 *
 * Commands: 73 / 623 wrapped (dlsym'd + called somewhere above).
 * Types:    85 / 2301 referenced (structs/enums/handles/flags/funcptrs
 *           this plugin names directly; excludes extension-only types this
 *           file never spells even if functionally implied).
 *
 * [x] = used by this plugin.  [ ] = not wrapped -- most are extension/
 * platform/ray-tracing/video-coding/compute-only surface irrelevant to the
 * curated App/Buffer/Shader/Pipeline API this file exposes to funcbuild (see
 * the top-of-file doc comment); this list exists as an at-a-glance coverage
 * map for anyone extending fbvk, not a promise to eventually wrap all of it.
 * ============================================================================
 *
 * ---- Commands (623) ----------------------------------------------------
 * [ ] vkAcquireDrmDisplayEXT
 * [ ] vkAcquireNextImage2KHR
 * [x] vkAcquireNextImageKHR
 * [ ] vkAcquirePerformanceConfigurationINTEL
 * [ ] vkAcquireProfilingLockKHR
 * [x] vkAllocateCommandBuffers
 * [ ] vkAllocateDescriptorSets
 * [x] vkAllocateMemory
 * [x] vkBeginCommandBuffer
 * [ ] vkBindAccelerationStructureMemoryNV
 * [x] vkBindBufferMemory
 * [ ] vkBindBufferMemory2
 * [ ] vkBindBufferMemory2KHR
 * [ ] vkBindImageMemory
 * [ ] vkBindImageMemory2
 * [ ] vkBindImageMemory2KHR
 * [ ] vkBindOpticalFlowSessionImageNV
 * [ ] vkBindVideoSessionMemoryKHR
 * [ ] vkBuildAccelerationStructuresKHR
 * [ ] vkBuildMicromapsEXT
 * [ ] vkCmdBeginConditionalRenderingEXT
 * [ ] vkCmdBeginDebugUtilsLabelEXT
 * [ ] vkCmdBeginQuery
 * [ ] vkCmdBeginQueryIndexedEXT
 * [ ] vkCmdBeginRendering
 * [ ] vkCmdBeginRenderingKHR
 * [x] vkCmdBeginRenderPass
 * [ ] vkCmdBeginRenderPass2
 * [ ] vkCmdBeginRenderPass2KHR
 * [ ] vkCmdBeginTransformFeedbackEXT
 * [ ] vkCmdBeginVideoCodingKHR
 * [ ] vkCmdBindDescriptorBufferEmbeddedSamplers2EXT
 * [ ] vkCmdBindDescriptorBufferEmbeddedSamplersEXT
 * [ ] vkCmdBindDescriptorBuffersEXT
 * [ ] vkCmdBindDescriptorSets
 * [ ] vkCmdBindDescriptorSets2KHR
 * [x] vkCmdBindIndexBuffer
 * [ ] vkCmdBindIndexBuffer2KHR
 * [ ] vkCmdBindInvocationMaskHUAWEI
 * [x] vkCmdBindPipeline
 * [ ] vkCmdBindPipelineShaderGroupNV
 * [ ] vkCmdBindShadersEXT
 * [ ] vkCmdBindShadingRateImageNV
 * [ ] vkCmdBindTransformFeedbackBuffersEXT
 * [x] vkCmdBindVertexBuffers
 * [ ] vkCmdBindVertexBuffers2
 * [ ] vkCmdBindVertexBuffers2EXT
 * [ ] vkCmdBlitImage
 * [ ] vkCmdBlitImage2
 * [ ] vkCmdBlitImage2KHR
 * [ ] vkCmdBuildAccelerationStructureNV
 * [ ] vkCmdBuildAccelerationStructuresIndirectKHR
 * [ ] vkCmdBuildAccelerationStructuresKHR
 * [ ] vkCmdBuildMicromapsEXT
 * [ ] vkCmdClearAttachments
 * [ ] vkCmdClearColorImage
 * [ ] vkCmdClearDepthStencilImage
 * [ ] vkCmdControlVideoCodingKHR
 * [ ] vkCmdCopyAccelerationStructureKHR
 * [ ] vkCmdCopyAccelerationStructureNV
 * [ ] vkCmdCopyAccelerationStructureToMemoryKHR
 * [x] vkCmdCopyBuffer
 * [ ] vkCmdCopyBuffer2
 * [ ] vkCmdCopyBuffer2KHR
 * [ ] vkCmdCopyBufferToImage
 * [ ] vkCmdCopyBufferToImage2
 * [ ] vkCmdCopyBufferToImage2KHR
 * [ ] vkCmdCopyImage
 * [ ] vkCmdCopyImage2
 * [ ] vkCmdCopyImage2KHR
 * [ ] vkCmdCopyImageToBuffer
 * [ ] vkCmdCopyImageToBuffer2
 * [ ] vkCmdCopyImageToBuffer2KHR
 * [ ] vkCmdCopyMemoryIndirectNV
 * [ ] vkCmdCopyMemoryToAccelerationStructureKHR
 * [ ] vkCmdCopyMemoryToImageIndirectNV
 * [ ] vkCmdCopyMemoryToMicromapEXT
 * [ ] vkCmdCopyMicromapEXT
 * [ ] vkCmdCopyMicromapToMemoryEXT
 * [ ] vkCmdCopyQueryPoolResults
 * [ ] vkCmdCudaLaunchKernelNV
 * [ ] vkCmdCuLaunchKernelNVX
 * [ ] vkCmdDebugMarkerBeginEXT
 * [ ] vkCmdDebugMarkerEndEXT
 * [ ] vkCmdDebugMarkerInsertEXT
 * [ ] vkCmdDecodeVideoKHR
 * [ ] vkCmdDecompressMemoryIndirectCountNV
 * [ ] vkCmdDecompressMemoryNV
 * [ ] vkCmdDispatch
 * [ ] vkCmdDispatchBase
 * [ ] vkCmdDispatchBaseKHR
 * [ ] vkCmdDispatchIndirect
 * [x] vkCmdDraw
 * [ ] vkCmdDrawClusterHUAWEI
 * [ ] vkCmdDrawClusterIndirectHUAWEI
 * [x] vkCmdDrawIndexed
 * [ ] vkCmdDrawIndexedIndirect
 * [ ] vkCmdDrawIndexedIndirectCount
 * [ ] vkCmdDrawIndexedIndirectCountAMD
 * [ ] vkCmdDrawIndexedIndirectCountKHR
 * [ ] vkCmdDrawIndirect
 * [ ] vkCmdDrawIndirectByteCountEXT
 * [ ] vkCmdDrawIndirectCount
 * [ ] vkCmdDrawIndirectCountAMD
 * [ ] vkCmdDrawIndirectCountKHR
 * [ ] vkCmdDrawMeshTasksEXT
 * [ ] vkCmdDrawMeshTasksIndirectCountEXT
 * [ ] vkCmdDrawMeshTasksIndirectCountNV
 * [ ] vkCmdDrawMeshTasksIndirectEXT
 * [ ] vkCmdDrawMeshTasksIndirectNV
 * [ ] vkCmdDrawMeshTasksNV
 * [ ] vkCmdDrawMultiEXT
 * [ ] vkCmdDrawMultiIndexedEXT
 * [ ] vkCmdEncodeVideoKHR
 * [ ] vkCmdEndConditionalRenderingEXT
 * [ ] vkCmdEndDebugUtilsLabelEXT
 * [ ] vkCmdEndQuery
 * [ ] vkCmdEndQueryIndexedEXT
 * [ ] vkCmdEndRendering
 * [ ] vkCmdEndRenderingKHR
 * [x] vkCmdEndRenderPass
 * [ ] vkCmdEndRenderPass2
 * [ ] vkCmdEndRenderPass2KHR
 * [ ] vkCmdEndTransformFeedbackEXT
 * [ ] vkCmdEndVideoCodingKHR
 * [ ] vkCmdExecuteCommands
 * [ ] vkCmdExecuteGeneratedCommandsNV
 * [x] vkCmdFillBuffer
 * [ ] vkCmdInsertDebugUtilsLabelEXT
 * [ ] vkCmdNextSubpass
 * [ ] vkCmdNextSubpass2
 * [ ] vkCmdNextSubpass2KHR
 * [ ] vkCmdOpticalFlowExecuteNV
 * [ ] vkCmdPipelineBarrier
 * [ ] vkCmdPipelineBarrier2
 * [ ] vkCmdPipelineBarrier2KHR
 * [ ] vkCmdPreprocessGeneratedCommandsNV
 * [ ] vkCmdPushConstants
 * [ ] vkCmdPushConstants2KHR
 * [ ] vkCmdPushDescriptorSet2KHR
 * [ ] vkCmdPushDescriptorSetKHR
 * [ ] vkCmdPushDescriptorSetWithTemplate2KHR
 * [ ] vkCmdPushDescriptorSetWithTemplateKHR
 * [ ] vkCmdResetEvent
 * [ ] vkCmdResetEvent2
 * [ ] vkCmdResetEvent2KHR
 * [ ] vkCmdResetQueryPool
 * [ ] vkCmdResolveImage
 * [ ] vkCmdResolveImage2
 * [ ] vkCmdResolveImage2KHR
 * [ ] vkCmdSetAlphaToCoverageEnableEXT
 * [ ] vkCmdSetAlphaToOneEnableEXT
 * [ ] vkCmdSetAttachmentFeedbackLoopEnableEXT
 * [ ] vkCmdSetBlendConstants
 * [ ] vkCmdSetCheckpointNV
 * [ ] vkCmdSetCoarseSampleOrderNV
 * [ ] vkCmdSetColorBlendAdvancedEXT
 * [ ] vkCmdSetColorBlendEnableEXT
 * [ ] vkCmdSetColorBlendEquationEXT
 * [ ] vkCmdSetColorWriteEnableEXT
 * [ ] vkCmdSetColorWriteMaskEXT
 * [ ] vkCmdSetConservativeRasterizationModeEXT
 * [ ] vkCmdSetCoverageModulationModeNV
 * [ ] vkCmdSetCoverageModulationTableEnableNV
 * [ ] vkCmdSetCoverageModulationTableNV
 * [ ] vkCmdSetCoverageReductionModeNV
 * [ ] vkCmdSetCoverageToColorEnableNV
 * [ ] vkCmdSetCoverageToColorLocationNV
 * [ ] vkCmdSetCullMode
 * [ ] vkCmdSetCullModeEXT
 * [ ] vkCmdSetDepthBias
 * [ ] vkCmdSetDepthBias2EXT
 * [ ] vkCmdSetDepthBiasEnable
 * [ ] vkCmdSetDepthBiasEnableEXT
 * [ ] vkCmdSetDepthBounds
 * [ ] vkCmdSetDepthBoundsTestEnable
 * [ ] vkCmdSetDepthBoundsTestEnableEXT
 * [ ] vkCmdSetDepthClampEnableEXT
 * [ ] vkCmdSetDepthClipEnableEXT
 * [ ] vkCmdSetDepthClipNegativeOneToOneEXT
 * [ ] vkCmdSetDepthCompareOp
 * [ ] vkCmdSetDepthCompareOpEXT
 * [ ] vkCmdSetDepthTestEnable
 * [ ] vkCmdSetDepthTestEnableEXT
 * [ ] vkCmdSetDepthWriteEnable
 * [ ] vkCmdSetDepthWriteEnableEXT
 * [ ] vkCmdSetDescriptorBufferOffsets2EXT
 * [ ] vkCmdSetDescriptorBufferOffsetsEXT
 * [ ] vkCmdSetDeviceMask
 * [ ] vkCmdSetDeviceMaskKHR
 * [ ] vkCmdSetDiscardRectangleEnableEXT
 * [ ] vkCmdSetDiscardRectangleEXT
 * [ ] vkCmdSetDiscardRectangleModeEXT
 * [ ] vkCmdSetEvent
 * [ ] vkCmdSetEvent2
 * [ ] vkCmdSetEvent2KHR
 * [ ] vkCmdSetExclusiveScissorEnableNV
 * [ ] vkCmdSetExclusiveScissorNV
 * [ ] vkCmdSetExtraPrimitiveOverestimationSizeEXT
 * [ ] vkCmdSetFragmentShadingRateEnumNV
 * [ ] vkCmdSetFragmentShadingRateKHR
 * [ ] vkCmdSetFrontFace
 * [ ] vkCmdSetFrontFaceEXT
 * [ ] vkCmdSetLineRasterizationModeEXT
 * [ ] vkCmdSetLineStippleEnableEXT
 * [ ] vkCmdSetLineStippleEXT
 * [ ] vkCmdSetLineWidth
 * [ ] vkCmdSetLogicOpEnableEXT
 * [ ] vkCmdSetLogicOpEXT
 * [ ] vkCmdSetPatchControlPointsEXT
 * [ ] vkCmdSetPerformanceMarkerINTEL
 * [ ] vkCmdSetPerformanceOverrideINTEL
 * [ ] vkCmdSetPerformanceStreamMarkerINTEL
 * [ ] vkCmdSetPolygonModeEXT
 * [ ] vkCmdSetPrimitiveRestartEnable
 * [ ] vkCmdSetPrimitiveRestartEnableEXT
 * [ ] vkCmdSetPrimitiveTopology
 * [ ] vkCmdSetPrimitiveTopologyEXT
 * [ ] vkCmdSetProvokingVertexModeEXT
 * [ ] vkCmdSetRasterizationSamplesEXT
 * [ ] vkCmdSetRasterizationStreamEXT
 * [ ] vkCmdSetRasterizerDiscardEnable
 * [ ] vkCmdSetRasterizerDiscardEnableEXT
 * [ ] vkCmdSetRayTracingPipelineStackSizeKHR
 * [ ] vkCmdSetRepresentativeFragmentTestEnableNV
 * [ ] vkCmdSetSampleLocationsEnableEXT
 * [ ] vkCmdSetSampleLocationsEXT
 * [ ] vkCmdSetSampleMaskEXT
 * [x] vkCmdSetScissor
 * [ ] vkCmdSetScissorWithCount
 * [ ] vkCmdSetScissorWithCountEXT
 * [ ] vkCmdSetShadingRateImageEnableNV
 * [ ] vkCmdSetStencilCompareMask
 * [ ] vkCmdSetStencilOp
 * [ ] vkCmdSetStencilOpEXT
 * [ ] vkCmdSetStencilReference
 * [ ] vkCmdSetStencilTestEnable
 * [ ] vkCmdSetStencilTestEnableEXT
 * [ ] vkCmdSetStencilWriteMask
 * [ ] vkCmdSetTessellationDomainOriginEXT
 * [ ] vkCmdSetVertexInputEXT
 * [x] vkCmdSetViewport
 * [ ] vkCmdSetViewportShadingRatePaletteNV
 * [ ] vkCmdSetViewportSwizzleNV
 * [ ] vkCmdSetViewportWithCount
 * [ ] vkCmdSetViewportWithCountEXT
 * [ ] vkCmdSetViewportWScalingEnableNV
 * [ ] vkCmdSetViewportWScalingNV
 * [ ] vkCmdSubpassShadingHUAWEI
 * [ ] vkCmdTraceRaysIndirect2KHR
 * [ ] vkCmdTraceRaysIndirectKHR
 * [ ] vkCmdTraceRaysKHR
 * [ ] vkCmdTraceRaysNV
 * [ ] vkCmdUpdateBuffer
 * [ ] vkCmdUpdatePipelineIndirectBufferNV
 * [ ] vkCmdWaitEvents
 * [ ] vkCmdWaitEvents2
 * [ ] vkCmdWaitEvents2KHR
 * [ ] vkCmdWriteAccelerationStructuresPropertiesKHR
 * [ ] vkCmdWriteAccelerationStructuresPropertiesNV
 * [ ] vkCmdWriteBufferMarker2AMD
 * [ ] vkCmdWriteBufferMarkerAMD
 * [ ] vkCmdWriteMicromapsPropertiesEXT
 * [ ] vkCmdWriteTimestamp
 * [ ] vkCmdWriteTimestamp2
 * [ ] vkCmdWriteTimestamp2KHR
 * [ ] vkCompileDeferredNV
 * [ ] vkCopyAccelerationStructureKHR
 * [ ] vkCopyAccelerationStructureToMemoryKHR
 * [ ] vkCopyImageToImageEXT
 * [ ] vkCopyImageToMemoryEXT
 * [ ] vkCopyMemoryToAccelerationStructureKHR
 * [ ] vkCopyMemoryToImageEXT
 * [ ] vkCopyMemoryToMicromapEXT
 * [ ] vkCopyMicromapEXT
 * [ ] vkCopyMicromapToMemoryEXT
 * [ ] vkCreateAccelerationStructureKHR
 * [ ] vkCreateAccelerationStructureNV
 * [x] vkCreateBuffer
 * [ ] vkCreateBufferView
 * [x] vkCreateCommandPool
 * [ ] vkCreateComputePipelines
 * [ ] vkCreateCudaFunctionNV
 * [ ] vkCreateCudaModuleNV
 * [ ] vkCreateCuFunctionNVX
 * [ ] vkCreateCuModuleNVX
 * [ ] vkCreateDebugReportCallbackEXT
 * [ ] vkCreateDebugUtilsMessengerEXT
 * [ ] vkCreateDeferredOperationKHR
 * [ ] vkCreateDescriptorPool
 * [ ] vkCreateDescriptorSetLayout
 * [ ] vkCreateDescriptorUpdateTemplate
 * [ ] vkCreateDescriptorUpdateTemplateKHR
 * [x] vkCreateDevice
 * [ ] vkCreateDisplayModeKHR
 * [ ] vkCreateDisplayPlaneSurfaceKHR
 * [ ] vkCreateEvent
 * [x] vkCreateFence
 * [x] vkCreateFramebuffer
 * [x] vkCreateGraphicsPipelines
 * [ ] vkCreateHeadlessSurfaceEXT
 * [ ] vkCreateImage
 * [x] vkCreateImageView
 * [ ] vkCreateIndirectCommandsLayoutNV
 * [x] vkCreateInstance
 * [ ] vkCreateMicromapEXT
 * [ ] vkCreateOpticalFlowSessionNV
 * [ ] vkCreatePipelineCache
 * [x] vkCreatePipelineLayout
 * [ ] vkCreatePrivateDataSlot
 * [ ] vkCreatePrivateDataSlotEXT
 * [ ] vkCreateQueryPool
 * [ ] vkCreateRayTracingPipelinesKHR
 * [ ] vkCreateRayTracingPipelinesNV
 * [x] vkCreateRenderPass
 * [ ] vkCreateRenderPass2
 * [ ] vkCreateRenderPass2KHR
 * [ ] vkCreateSampler
 * [ ] vkCreateSamplerYcbcrConversion
 * [ ] vkCreateSamplerYcbcrConversionKHR
 * [x] vkCreateSemaphore
 * [x] vkCreateShaderModule
 * [ ] vkCreateShadersEXT
 * [ ] vkCreateSharedSwapchainsKHR
 * [x] vkCreateSwapchainKHR
 * [ ] vkCreateValidationCacheEXT
 * [ ] vkCreateVideoSessionKHR
 * [ ] vkCreateVideoSessionParametersKHR
 * [ ] vkDebugMarkerSetObjectNameEXT
 * [ ] vkDebugMarkerSetObjectTagEXT
 * [ ] vkDebugReportMessageEXT
 * [ ] vkDeferredOperationJoinKHR
 * [ ] vkDestroyAccelerationStructureKHR
 * [ ] vkDestroyAccelerationStructureNV
 * [x] vkDestroyBuffer
 * [ ] vkDestroyBufferView
 * [x] vkDestroyCommandPool
 * [ ] vkDestroyCudaFunctionNV
 * [ ] vkDestroyCudaModuleNV
 * [ ] vkDestroyCuFunctionNVX
 * [ ] vkDestroyCuModuleNVX
 * [ ] vkDestroyDebugReportCallbackEXT
 * [ ] vkDestroyDebugUtilsMessengerEXT
 * [ ] vkDestroyDeferredOperationKHR
 * [ ] vkDestroyDescriptorPool
 * [ ] vkDestroyDescriptorSetLayout
 * [ ] vkDestroyDescriptorUpdateTemplate
 * [ ] vkDestroyDescriptorUpdateTemplateKHR
 * [x] vkDestroyDevice
 * [ ] vkDestroyEvent
 * [x] vkDestroyFence
 * [x] vkDestroyFramebuffer
 * [ ] vkDestroyImage
 * [x] vkDestroyImageView
 * [ ] vkDestroyIndirectCommandsLayoutNV
 * [x] vkDestroyInstance
 * [ ] vkDestroyMicromapEXT
 * [ ] vkDestroyOpticalFlowSessionNV
 * [x] vkDestroyPipeline
 * [ ] vkDestroyPipelineCache
 * [x] vkDestroyPipelineLayout
 * [ ] vkDestroyPrivateDataSlot
 * [ ] vkDestroyPrivateDataSlotEXT
 * [ ] vkDestroyQueryPool
 * [x] vkDestroyRenderPass
 * [ ] vkDestroySampler
 * [ ] vkDestroySamplerYcbcrConversion
 * [ ] vkDestroySamplerYcbcrConversionKHR
 * [x] vkDestroySemaphore
 * [ ] vkDestroyShaderEXT
 * [x] vkDestroyShaderModule
 * [x] vkDestroySurfaceKHR
 * [x] vkDestroySwapchainKHR
 * [ ] vkDestroyValidationCacheEXT
 * [ ] vkDestroyVideoSessionKHR
 * [ ] vkDestroyVideoSessionParametersKHR
 * [x] vkDeviceWaitIdle
 * [ ] vkDisplayPowerControlEXT
 * [x] vkEndCommandBuffer
 * [ ] vkEnumerateDeviceExtensionProperties
 * [ ] vkEnumerateDeviceLayerProperties
 * [ ] vkEnumerateInstanceExtensionProperties
 * [x] vkEnumerateInstanceLayerProperties
 * [x] vkEnumerateInstanceVersion
 * [ ] vkEnumeratePhysicalDeviceGroups
 * [ ] vkEnumeratePhysicalDeviceGroupsKHR
 * [ ] vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR
 * [x] vkEnumeratePhysicalDevices
 * [ ] vkFlushMappedMemoryRanges
 * [x] vkFreeCommandBuffers
 * [ ] vkFreeDescriptorSets
 * [x] vkFreeMemory
 * [ ] vkGetAccelerationStructureBuildSizesKHR
 * [ ] vkGetAccelerationStructureDeviceAddressKHR
 * [ ] vkGetAccelerationStructureHandleNV
 * [ ] vkGetAccelerationStructureMemoryRequirementsNV
 * [ ] vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT
 * [ ] vkGetBufferDeviceAddress
 * [ ] vkGetBufferDeviceAddressEXT
 * [ ] vkGetBufferDeviceAddressKHR
 * [x] vkGetBufferMemoryRequirements
 * [ ] vkGetBufferMemoryRequirements2
 * [ ] vkGetBufferMemoryRequirements2KHR
 * [ ] vkGetBufferOpaqueCaptureAddress
 * [ ] vkGetBufferOpaqueCaptureAddressKHR
 * [ ] vkGetBufferOpaqueCaptureDescriptorDataEXT
 * [ ] vkGetCalibratedTimestampsEXT
 * [ ] vkGetCalibratedTimestampsKHR
 * [ ] vkGetCudaModuleCacheNV
 * [ ] vkGetDeferredOperationMaxConcurrencyKHR
 * [ ] vkGetDeferredOperationResultKHR
 * [ ] vkGetDescriptorEXT
 * [ ] vkGetDescriptorSetHostMappingVALVE
 * [ ] vkGetDescriptorSetLayoutBindingOffsetEXT
 * [ ] vkGetDescriptorSetLayoutHostMappingInfoVALVE
 * [ ] vkGetDescriptorSetLayoutSizeEXT
 * [ ] vkGetDescriptorSetLayoutSupport
 * [ ] vkGetDescriptorSetLayoutSupportKHR
 * [ ] vkGetDeviceAccelerationStructureCompatibilityKHR
 * [ ] vkGetDeviceBufferMemoryRequirements
 * [ ] vkGetDeviceBufferMemoryRequirementsKHR
 * [ ] vkGetDeviceFaultInfoEXT
 * [ ] vkGetDeviceGroupPeerMemoryFeatures
 * [ ] vkGetDeviceGroupPeerMemoryFeaturesKHR
 * [ ] vkGetDeviceGroupPresentCapabilitiesKHR
 * [ ] vkGetDeviceGroupSurfacePresentModesKHR
 * [ ] vkGetDeviceImageMemoryRequirements
 * [ ] vkGetDeviceImageMemoryRequirementsKHR
 * [ ] vkGetDeviceImageSparseMemoryRequirements
 * [ ] vkGetDeviceImageSparseMemoryRequirementsKHR
 * [ ] vkGetDeviceImageSubresourceLayoutKHR
 * [ ] vkGetDeviceMemoryCommitment
 * [ ] vkGetDeviceMemoryOpaqueCaptureAddress
 * [ ] vkGetDeviceMemoryOpaqueCaptureAddressKHR
 * [ ] vkGetDeviceMicromapCompatibilityEXT
 * [ ] vkGetDeviceProcAddr
 * [x] vkGetDeviceQueue
 * [ ] vkGetDeviceQueue2
 * [ ] vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI
 * [ ] vkGetDisplayModeProperties2KHR
 * [ ] vkGetDisplayModePropertiesKHR
 * [ ] vkGetDisplayPlaneCapabilities2KHR
 * [ ] vkGetDisplayPlaneCapabilitiesKHR
 * [ ] vkGetDisplayPlaneSupportedDisplaysKHR
 * [ ] vkGetDrmDisplayEXT
 * [ ] vkGetDynamicRenderingTilePropertiesQCOM
 * [ ] vkGetEncodedVideoSessionParametersKHR
 * [ ] vkGetEventStatus
 * [ ] vkGetFenceFdKHR
 * [x] vkGetFenceStatus
 * [ ] vkGetFramebufferTilePropertiesQCOM
 * [ ] vkGetGeneratedCommandsMemoryRequirementsNV
 * [ ] vkGetImageDrmFormatModifierPropertiesEXT
 * [ ] vkGetImageMemoryRequirements
 * [ ] vkGetImageMemoryRequirements2
 * [ ] vkGetImageMemoryRequirements2KHR
 * [ ] vkGetImageOpaqueCaptureDescriptorDataEXT
 * [ ] vkGetImageSparseMemoryRequirements
 * [ ] vkGetImageSparseMemoryRequirements2
 * [ ] vkGetImageSparseMemoryRequirements2KHR
 * [ ] vkGetImageSubresourceLayout
 * [ ] vkGetImageSubresourceLayout2EXT
 * [ ] vkGetImageSubresourceLayout2KHR
 * [ ] vkGetImageViewAddressNVX
 * [ ] vkGetImageViewHandleNVX
 * [ ] vkGetImageViewOpaqueCaptureDescriptorDataEXT
 * [x] vkGetInstanceProcAddr
 * [ ] vkGetLatencyTimingsNV
 * [ ] vkGetMemoryFdKHR
 * [ ] vkGetMemoryFdPropertiesKHR
 * [ ] vkGetMemoryHostPointerPropertiesEXT
 * [ ] vkGetMemoryRemoteAddressNV
 * [ ] vkGetMicromapBuildSizesEXT
 * [ ] vkGetPastPresentationTimingGOOGLE
 * [ ] vkGetPerformanceParameterINTEL
 * [ ] vkGetPhysicalDeviceCalibrateableTimeDomainsEXT
 * [ ] vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
 * [ ] vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR
 * [ ] vkGetPhysicalDeviceCooperativeMatrixPropertiesNV
 * [ ] vkGetPhysicalDeviceDisplayPlaneProperties2KHR
 * [ ] vkGetPhysicalDeviceDisplayPlanePropertiesKHR
 * [ ] vkGetPhysicalDeviceDisplayProperties2KHR
 * [ ] vkGetPhysicalDeviceDisplayPropertiesKHR
 * [ ] vkGetPhysicalDeviceExternalBufferProperties
 * [ ] vkGetPhysicalDeviceExternalBufferPropertiesKHR
 * [ ] vkGetPhysicalDeviceExternalFenceProperties
 * [ ] vkGetPhysicalDeviceExternalFencePropertiesKHR
 * [ ] vkGetPhysicalDeviceExternalImageFormatPropertiesNV
 * [ ] vkGetPhysicalDeviceExternalSemaphoreProperties
 * [ ] vkGetPhysicalDeviceExternalSemaphorePropertiesKHR
 * [x] vkGetPhysicalDeviceFeatures
 * [ ] vkGetPhysicalDeviceFeatures2
 * [ ] vkGetPhysicalDeviceFeatures2KHR
 * [x] vkGetPhysicalDeviceFormatProperties
 * [ ] vkGetPhysicalDeviceFormatProperties2
 * [ ] vkGetPhysicalDeviceFormatProperties2KHR
 * [ ] vkGetPhysicalDeviceFragmentShadingRatesKHR
 * [ ] vkGetPhysicalDeviceImageFormatProperties
 * [ ] vkGetPhysicalDeviceImageFormatProperties2
 * [ ] vkGetPhysicalDeviceImageFormatProperties2KHR
 * [x] vkGetPhysicalDeviceMemoryProperties
 * [ ] vkGetPhysicalDeviceMemoryProperties2
 * [ ] vkGetPhysicalDeviceMemoryProperties2KHR
 * [ ] vkGetPhysicalDeviceMultisamplePropertiesEXT
 * [ ] vkGetPhysicalDeviceOpticalFlowImageFormatsNV
 * [ ] vkGetPhysicalDevicePresentRectanglesKHR
 * [x] vkGetPhysicalDeviceProperties
 * [ ] vkGetPhysicalDeviceProperties2
 * [ ] vkGetPhysicalDeviceProperties2KHR
 * [ ] vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR
 * [x] vkGetPhysicalDeviceQueueFamilyProperties
 * [ ] vkGetPhysicalDeviceQueueFamilyProperties2
 * [ ] vkGetPhysicalDeviceQueueFamilyProperties2KHR
 * [ ] vkGetPhysicalDeviceSparseImageFormatProperties
 * [ ] vkGetPhysicalDeviceSparseImageFormatProperties2
 * [ ] vkGetPhysicalDeviceSparseImageFormatProperties2KHR
 * [ ] vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV
 * [ ] vkGetPhysicalDeviceSurfaceCapabilities2EXT
 * [ ] vkGetPhysicalDeviceSurfaceCapabilities2KHR
 * [x] vkGetPhysicalDeviceSurfaceCapabilitiesKHR
 * [ ] vkGetPhysicalDeviceSurfaceFormats2KHR
 * [x] vkGetPhysicalDeviceSurfaceFormatsKHR
 * [x] vkGetPhysicalDeviceSurfacePresentModesKHR
 * [x] vkGetPhysicalDeviceSurfaceSupportKHR
 * [ ] vkGetPhysicalDeviceToolProperties
 * [ ] vkGetPhysicalDeviceToolPropertiesEXT
 * [ ] vkGetPhysicalDeviceVideoCapabilitiesKHR
 * [ ] vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR
 * [ ] vkGetPhysicalDeviceVideoFormatPropertiesKHR
 * [ ] vkGetPipelineCacheData
 * [ ] vkGetPipelineExecutableInternalRepresentationsKHR
 * [ ] vkGetPipelineExecutablePropertiesKHR
 * [ ] vkGetPipelineExecutableStatisticsKHR
 * [ ] vkGetPipelineIndirectDeviceAddressNV
 * [ ] vkGetPipelineIndirectMemoryRequirementsNV
 * [ ] vkGetPipelinePropertiesEXT
 * [ ] vkGetPrivateData
 * [ ] vkGetPrivateDataEXT
 * [ ] vkGetQueryPoolResults
 * [ ] vkGetQueueCheckpointData2NV
 * [ ] vkGetQueueCheckpointDataNV
 * [ ] vkGetRayTracingCaptureReplayShaderGroupHandlesKHR
 * [ ] vkGetRayTracingShaderGroupHandlesKHR
 * [ ] vkGetRayTracingShaderGroupHandlesNV
 * [ ] vkGetRayTracingShaderGroupStackSizeKHR
 * [ ] vkGetRefreshCycleDurationGOOGLE
 * [ ] vkGetRenderAreaGranularity
 * [ ] vkGetRenderingAreaGranularityKHR
 * [ ] vkGetSamplerOpaqueCaptureDescriptorDataEXT
 * [ ] vkGetSemaphoreCounterValue
 * [ ] vkGetSemaphoreCounterValueKHR
 * [ ] vkGetSemaphoreFdKHR
 * [ ] vkGetShaderBinaryDataEXT
 * [ ] vkGetShaderInfoAMD
 * [ ] vkGetShaderModuleCreateInfoIdentifierEXT
 * [ ] vkGetShaderModuleIdentifierEXT
 * [ ] vkGetSwapchainCounterEXT
 * [x] vkGetSwapchainImagesKHR
 * [ ] vkGetSwapchainStatusKHR
 * [ ] vkGetValidationCacheDataEXT
 * [ ] vkGetVideoSessionMemoryRequirementsKHR
 * [ ] vkImportFenceFdKHR
 * [ ] vkImportSemaphoreFdKHR
 * [ ] vkInitializePerformanceApiINTEL
 * [ ] vkInvalidateMappedMemoryRanges
 * [ ] vkLatencySleepNV
 * [x] vkMapMemory
 * [ ] vkMapMemory2KHR
 * [ ] vkMergePipelineCaches
 * [ ] vkMergeValidationCachesEXT
 * [ ] vkQueueBeginDebugUtilsLabelEXT
 * [ ] vkQueueBindSparse
 * [ ] vkQueueEndDebugUtilsLabelEXT
 * [ ] vkQueueInsertDebugUtilsLabelEXT
 * [ ] vkQueueNotifyOutOfBandNV
 * [x] vkQueuePresentKHR
 * [ ] vkQueueSetPerformanceConfigurationINTEL
 * [x] vkQueueSubmit
 * [ ] vkQueueSubmit2
 * [ ] vkQueueSubmit2KHR
 * [x] vkQueueWaitIdle
 * [ ] vkRegisterDeviceEventEXT
 * [ ] vkRegisterDisplayEventEXT
 * [ ] vkReleaseDisplayEXT
 * [ ] vkReleasePerformanceConfigurationINTEL
 * [ ] vkReleaseProfilingLockKHR
 * [ ] vkReleaseSwapchainImagesEXT
 * [x] vkResetCommandBuffer
 * [x] vkResetCommandPool
 * [ ] vkResetDescriptorPool
 * [ ] vkResetEvent
 * [x] vkResetFences
 * [ ] vkResetQueryPool
 * [ ] vkResetQueryPoolEXT
 * [ ] vkSetDebugUtilsObjectNameEXT
 * [ ] vkSetDebugUtilsObjectTagEXT
 * [ ] vkSetDeviceMemoryPriorityEXT
 * [ ] vkSetEvent
 * [ ] vkSetHdrMetadataEXT
 * [ ] vkSetLatencyMarkerNV
 * [ ] vkSetLatencySleepModeNV
 * [ ] vkSetLocalDimmingAMD
 * [ ] vkSetPrivateData
 * [ ] vkSetPrivateDataEXT
 * [ ] vkSignalSemaphore
 * [ ] vkSignalSemaphoreKHR
 * [ ] vkSubmitDebugUtilsMessageEXT
 * [ ] vkTransitionImageLayoutEXT
 * [ ] vkTrimCommandPool
 * [ ] vkTrimCommandPoolKHR
 * [ ] vkUninitializePerformanceApiINTEL
 * [x] vkUnmapMemory
 * [ ] vkUnmapMemory2KHR
 * [ ] vkUpdateDescriptorSets
 * [ ] vkUpdateDescriptorSetWithTemplate
 * [ ] vkUpdateDescriptorSetWithTemplateKHR
 * [ ] vkUpdateVideoSessionParametersKHR
 * [x] vkWaitForFences
 * [ ] vkWaitForPresentKHR
 * [ ] vkWaitSemaphores
 * [ ] vkWaitSemaphoresKHR
 * [ ] vkWriteAccelerationStructuresPropertiesKHR
 * [ ] vkWriteMicromapsPropertiesEXT
 *
 * ---- Types (2301) --------------------------------------------------------
 * [ ] PFN_vkAcquireDrmDisplayEXT
 * [ ] PFN_vkAcquireNextImage2KHR
 * [ ] PFN_vkAcquireNextImageKHR
 * [ ] PFN_vkAcquirePerformanceConfigurationINTEL
 * [ ] PFN_vkAcquireProfilingLockKHR
 * [ ] PFN_vkAllocateCommandBuffers
 * [ ] PFN_vkAllocateDescriptorSets
 * [ ] PFN_vkAllocateMemory
 * [ ] PFN_vkAllocationFunction
 * [ ] PFN_vkBeginCommandBuffer
 * [ ] PFN_vkBindAccelerationStructureMemoryNV
 * [ ] PFN_vkBindBufferMemory
 * [ ] PFN_vkBindBufferMemory2
 * [ ] PFN_vkBindBufferMemory2KHR
 * [ ] PFN_vkBindImageMemory
 * [ ] PFN_vkBindImageMemory2
 * [ ] PFN_vkBindImageMemory2KHR
 * [ ] PFN_vkBindOpticalFlowSessionImageNV
 * [ ] PFN_vkBindVideoSessionMemoryKHR
 * [ ] PFN_vkBuildAccelerationStructuresKHR
 * [ ] PFN_vkBuildMicromapsEXT
 * [ ] PFN_vkCmdBeginConditionalRenderingEXT
 * [ ] PFN_vkCmdBeginDebugUtilsLabelEXT
 * [ ] PFN_vkCmdBeginQuery
 * [ ] PFN_vkCmdBeginQueryIndexedEXT
 * [ ] PFN_vkCmdBeginRendering
 * [ ] PFN_vkCmdBeginRenderingKHR
 * [ ] PFN_vkCmdBeginRenderPass
 * [ ] PFN_vkCmdBeginRenderPass2
 * [ ] PFN_vkCmdBeginRenderPass2KHR
 * [ ] PFN_vkCmdBeginTransformFeedbackEXT
 * [ ] PFN_vkCmdBeginVideoCodingKHR
 * [ ] PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT
 * [ ] PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT
 * [ ] PFN_vkCmdBindDescriptorBuffersEXT
 * [ ] PFN_vkCmdBindDescriptorSets
 * [ ] PFN_vkCmdBindDescriptorSets2KHR
 * [ ] PFN_vkCmdBindIndexBuffer
 * [ ] PFN_vkCmdBindIndexBuffer2KHR
 * [ ] PFN_vkCmdBindInvocationMaskHUAWEI
 * [ ] PFN_vkCmdBindPipeline
 * [ ] PFN_vkCmdBindPipelineShaderGroupNV
 * [ ] PFN_vkCmdBindShadersEXT
 * [ ] PFN_vkCmdBindShadingRateImageNV
 * [ ] PFN_vkCmdBindTransformFeedbackBuffersEXT
 * [ ] PFN_vkCmdBindVertexBuffers
 * [ ] PFN_vkCmdBindVertexBuffers2
 * [ ] PFN_vkCmdBindVertexBuffers2EXT
 * [ ] PFN_vkCmdBlitImage
 * [ ] PFN_vkCmdBlitImage2
 * [ ] PFN_vkCmdBlitImage2KHR
 * [ ] PFN_vkCmdBuildAccelerationStructureNV
 * [ ] PFN_vkCmdBuildAccelerationStructuresIndirectKHR
 * [ ] PFN_vkCmdBuildAccelerationStructuresKHR
 * [ ] PFN_vkCmdBuildMicromapsEXT
 * [ ] PFN_vkCmdClearAttachments
 * [ ] PFN_vkCmdClearColorImage
 * [ ] PFN_vkCmdClearDepthStencilImage
 * [ ] PFN_vkCmdControlVideoCodingKHR
 * [ ] PFN_vkCmdCopyAccelerationStructureKHR
 * [ ] PFN_vkCmdCopyAccelerationStructureNV
 * [ ] PFN_vkCmdCopyAccelerationStructureToMemoryKHR
 * [x] PFN_vkCmdCopyBuffer
 * [ ] PFN_vkCmdCopyBuffer2
 * [ ] PFN_vkCmdCopyBuffer2KHR
 * [ ] PFN_vkCmdCopyBufferToImage
 * [ ] PFN_vkCmdCopyBufferToImage2
 * [ ] PFN_vkCmdCopyBufferToImage2KHR
 * [ ] PFN_vkCmdCopyImage
 * [ ] PFN_vkCmdCopyImage2
 * [ ] PFN_vkCmdCopyImage2KHR
 * [ ] PFN_vkCmdCopyImageToBuffer
 * [ ] PFN_vkCmdCopyImageToBuffer2
 * [ ] PFN_vkCmdCopyImageToBuffer2KHR
 * [ ] PFN_vkCmdCopyMemoryIndirectNV
 * [ ] PFN_vkCmdCopyMemoryToAccelerationStructureKHR
 * [ ] PFN_vkCmdCopyMemoryToImageIndirectNV
 * [ ] PFN_vkCmdCopyMemoryToMicromapEXT
 * [ ] PFN_vkCmdCopyMicromapEXT
 * [ ] PFN_vkCmdCopyMicromapToMemoryEXT
 * [ ] PFN_vkCmdCopyQueryPoolResults
 * [ ] PFN_vkCmdCudaLaunchKernelNV
 * [ ] PFN_vkCmdCuLaunchKernelNVX
 * [ ] PFN_vkCmdDebugMarkerBeginEXT
 * [ ] PFN_vkCmdDebugMarkerEndEXT
 * [ ] PFN_vkCmdDebugMarkerInsertEXT
 * [ ] PFN_vkCmdDecodeVideoKHR
 * [ ] PFN_vkCmdDecompressMemoryIndirectCountNV
 * [ ] PFN_vkCmdDecompressMemoryNV
 * [ ] PFN_vkCmdDispatch
 * [ ] PFN_vkCmdDispatchBase
 * [ ] PFN_vkCmdDispatchBaseKHR
 * [ ] PFN_vkCmdDispatchIndirect
 * [ ] PFN_vkCmdDraw
 * [ ] PFN_vkCmdDrawClusterHUAWEI
 * [ ] PFN_vkCmdDrawClusterIndirectHUAWEI
 * [ ] PFN_vkCmdDrawIndexed
 * [ ] PFN_vkCmdDrawIndexedIndirect
 * [ ] PFN_vkCmdDrawIndexedIndirectCount
 * [ ] PFN_vkCmdDrawIndexedIndirectCountAMD
 * [ ] PFN_vkCmdDrawIndexedIndirectCountKHR
 * [ ] PFN_vkCmdDrawIndirect
 * [ ] PFN_vkCmdDrawIndirectByteCountEXT
 * [ ] PFN_vkCmdDrawIndirectCount
 * [ ] PFN_vkCmdDrawIndirectCountAMD
 * [ ] PFN_vkCmdDrawIndirectCountKHR
 * [ ] PFN_vkCmdDrawMeshTasksEXT
 * [ ] PFN_vkCmdDrawMeshTasksIndirectCountEXT
 * [ ] PFN_vkCmdDrawMeshTasksIndirectCountNV
 * [ ] PFN_vkCmdDrawMeshTasksIndirectEXT
 * [ ] PFN_vkCmdDrawMeshTasksIndirectNV
 * [ ] PFN_vkCmdDrawMeshTasksNV
 * [ ] PFN_vkCmdDrawMultiEXT
 * [ ] PFN_vkCmdDrawMultiIndexedEXT
 * [ ] PFN_vkCmdEncodeVideoKHR
 * [ ] PFN_vkCmdEndConditionalRenderingEXT
 * [ ] PFN_vkCmdEndDebugUtilsLabelEXT
 * [ ] PFN_vkCmdEndQuery
 * [ ] PFN_vkCmdEndQueryIndexedEXT
 * [ ] PFN_vkCmdEndRendering
 * [ ] PFN_vkCmdEndRenderingKHR
 * [ ] PFN_vkCmdEndRenderPass
 * [ ] PFN_vkCmdEndRenderPass2
 * [ ] PFN_vkCmdEndRenderPass2KHR
 * [ ] PFN_vkCmdEndTransformFeedbackEXT
 * [ ] PFN_vkCmdEndVideoCodingKHR
 * [ ] PFN_vkCmdExecuteCommands
 * [ ] PFN_vkCmdExecuteGeneratedCommandsNV
 * [x] PFN_vkCmdFillBuffer
 * [ ] PFN_vkCmdInsertDebugUtilsLabelEXT
 * [ ] PFN_vkCmdNextSubpass
 * [ ] PFN_vkCmdNextSubpass2
 * [ ] PFN_vkCmdNextSubpass2KHR
 * [ ] PFN_vkCmdOpticalFlowExecuteNV
 * [ ] PFN_vkCmdPipelineBarrier
 * [ ] PFN_vkCmdPipelineBarrier2
 * [ ] PFN_vkCmdPipelineBarrier2KHR
 * [ ] PFN_vkCmdPreprocessGeneratedCommandsNV
 * [ ] PFN_vkCmdPushConstants
 * [ ] PFN_vkCmdPushConstants2KHR
 * [ ] PFN_vkCmdPushDescriptorSet2KHR
 * [ ] PFN_vkCmdPushDescriptorSetKHR
 * [ ] PFN_vkCmdPushDescriptorSetWithTemplate2KHR
 * [ ] PFN_vkCmdPushDescriptorSetWithTemplateKHR
 * [ ] PFN_vkCmdResetEvent
 * [ ] PFN_vkCmdResetEvent2
 * [ ] PFN_vkCmdResetEvent2KHR
 * [ ] PFN_vkCmdResetQueryPool
 * [ ] PFN_vkCmdResolveImage
 * [ ] PFN_vkCmdResolveImage2
 * [ ] PFN_vkCmdResolveImage2KHR
 * [ ] PFN_vkCmdSetAlphaToCoverageEnableEXT
 * [ ] PFN_vkCmdSetAlphaToOneEnableEXT
 * [ ] PFN_vkCmdSetAttachmentFeedbackLoopEnableEXT
 * [ ] PFN_vkCmdSetBlendConstants
 * [ ] PFN_vkCmdSetCheckpointNV
 * [ ] PFN_vkCmdSetCoarseSampleOrderNV
 * [ ] PFN_vkCmdSetColorBlendAdvancedEXT
 * [ ] PFN_vkCmdSetColorBlendEnableEXT
 * [ ] PFN_vkCmdSetColorBlendEquationEXT
 * [ ] PFN_vkCmdSetColorWriteEnableEXT
 * [ ] PFN_vkCmdSetColorWriteMaskEXT
 * [ ] PFN_vkCmdSetConservativeRasterizationModeEXT
 * [ ] PFN_vkCmdSetCoverageModulationModeNV
 * [ ] PFN_vkCmdSetCoverageModulationTableEnableNV
 * [ ] PFN_vkCmdSetCoverageModulationTableNV
 * [ ] PFN_vkCmdSetCoverageReductionModeNV
 * [ ] PFN_vkCmdSetCoverageToColorEnableNV
 * [ ] PFN_vkCmdSetCoverageToColorLocationNV
 * [ ] PFN_vkCmdSetCullMode
 * [ ] PFN_vkCmdSetCullModeEXT
 * [ ] PFN_vkCmdSetDepthBias
 * [ ] PFN_vkCmdSetDepthBias2EXT
 * [ ] PFN_vkCmdSetDepthBiasEnable
 * [ ] PFN_vkCmdSetDepthBiasEnableEXT
 * [ ] PFN_vkCmdSetDepthBounds
 * [ ] PFN_vkCmdSetDepthBoundsTestEnable
 * [ ] PFN_vkCmdSetDepthBoundsTestEnableEXT
 * [ ] PFN_vkCmdSetDepthClampEnableEXT
 * [ ] PFN_vkCmdSetDepthClipEnableEXT
 * [ ] PFN_vkCmdSetDepthClipNegativeOneToOneEXT
 * [ ] PFN_vkCmdSetDepthCompareOp
 * [ ] PFN_vkCmdSetDepthCompareOpEXT
 * [ ] PFN_vkCmdSetDepthTestEnable
 * [ ] PFN_vkCmdSetDepthTestEnableEXT
 * [ ] PFN_vkCmdSetDepthWriteEnable
 * [ ] PFN_vkCmdSetDepthWriteEnableEXT
 * [ ] PFN_vkCmdSetDescriptorBufferOffsets2EXT
 * [ ] PFN_vkCmdSetDescriptorBufferOffsetsEXT
 * [ ] PFN_vkCmdSetDeviceMask
 * [ ] PFN_vkCmdSetDeviceMaskKHR
 * [ ] PFN_vkCmdSetDiscardRectangleEnableEXT
 * [ ] PFN_vkCmdSetDiscardRectangleEXT
 * [ ] PFN_vkCmdSetDiscardRectangleModeEXT
 * [ ] PFN_vkCmdSetEvent
 * [ ] PFN_vkCmdSetEvent2
 * [ ] PFN_vkCmdSetEvent2KHR
 * [ ] PFN_vkCmdSetExclusiveScissorEnableNV
 * [ ] PFN_vkCmdSetExclusiveScissorNV
 * [ ] PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT
 * [ ] PFN_vkCmdSetFragmentShadingRateEnumNV
 * [ ] PFN_vkCmdSetFragmentShadingRateKHR
 * [ ] PFN_vkCmdSetFrontFace
 * [ ] PFN_vkCmdSetFrontFaceEXT
 * [ ] PFN_vkCmdSetLineRasterizationModeEXT
 * [ ] PFN_vkCmdSetLineStippleEnableEXT
 * [ ] PFN_vkCmdSetLineStippleEXT
 * [ ] PFN_vkCmdSetLineWidth
 * [ ] PFN_vkCmdSetLogicOpEnableEXT
 * [ ] PFN_vkCmdSetLogicOpEXT
 * [ ] PFN_vkCmdSetPatchControlPointsEXT
 * [ ] PFN_vkCmdSetPerformanceMarkerINTEL
 * [ ] PFN_vkCmdSetPerformanceOverrideINTEL
 * [ ] PFN_vkCmdSetPerformanceStreamMarkerINTEL
 * [ ] PFN_vkCmdSetPolygonModeEXT
 * [ ] PFN_vkCmdSetPrimitiveRestartEnable
 * [ ] PFN_vkCmdSetPrimitiveRestartEnableEXT
 * [ ] PFN_vkCmdSetPrimitiveTopology
 * [ ] PFN_vkCmdSetPrimitiveTopologyEXT
 * [ ] PFN_vkCmdSetProvokingVertexModeEXT
 * [ ] PFN_vkCmdSetRasterizationSamplesEXT
 * [ ] PFN_vkCmdSetRasterizationStreamEXT
 * [ ] PFN_vkCmdSetRasterizerDiscardEnable
 * [ ] PFN_vkCmdSetRasterizerDiscardEnableEXT
 * [ ] PFN_vkCmdSetRayTracingPipelineStackSizeKHR
 * [ ] PFN_vkCmdSetRepresentativeFragmentTestEnableNV
 * [ ] PFN_vkCmdSetSampleLocationsEnableEXT
 * [ ] PFN_vkCmdSetSampleLocationsEXT
 * [ ] PFN_vkCmdSetSampleMaskEXT
 * [ ] PFN_vkCmdSetScissor
 * [ ] PFN_vkCmdSetScissorWithCount
 * [ ] PFN_vkCmdSetScissorWithCountEXT
 * [ ] PFN_vkCmdSetShadingRateImageEnableNV
 * [ ] PFN_vkCmdSetStencilCompareMask
 * [ ] PFN_vkCmdSetStencilOp
 * [ ] PFN_vkCmdSetStencilOpEXT
 * [ ] PFN_vkCmdSetStencilReference
 * [ ] PFN_vkCmdSetStencilTestEnable
 * [ ] PFN_vkCmdSetStencilTestEnableEXT
 * [ ] PFN_vkCmdSetStencilWriteMask
 * [ ] PFN_vkCmdSetTessellationDomainOriginEXT
 * [ ] PFN_vkCmdSetVertexInputEXT
 * [ ] PFN_vkCmdSetViewport
 * [ ] PFN_vkCmdSetViewportShadingRatePaletteNV
 * [ ] PFN_vkCmdSetViewportSwizzleNV
 * [ ] PFN_vkCmdSetViewportWithCount
 * [ ] PFN_vkCmdSetViewportWithCountEXT
 * [ ] PFN_vkCmdSetViewportWScalingEnableNV
 * [ ] PFN_vkCmdSetViewportWScalingNV
 * [ ] PFN_vkCmdSubpassShadingHUAWEI
 * [ ] PFN_vkCmdTraceRaysIndirect2KHR
 * [ ] PFN_vkCmdTraceRaysIndirectKHR
 * [ ] PFN_vkCmdTraceRaysKHR
 * [ ] PFN_vkCmdTraceRaysNV
 * [ ] PFN_vkCmdUpdateBuffer
 * [ ] PFN_vkCmdUpdatePipelineIndirectBufferNV
 * [ ] PFN_vkCmdWaitEvents
 * [ ] PFN_vkCmdWaitEvents2
 * [ ] PFN_vkCmdWaitEvents2KHR
 * [ ] PFN_vkCmdWriteAccelerationStructuresPropertiesKHR
 * [ ] PFN_vkCmdWriteAccelerationStructuresPropertiesNV
 * [ ] PFN_vkCmdWriteBufferMarker2AMD
 * [ ] PFN_vkCmdWriteBufferMarkerAMD
 * [ ] PFN_vkCmdWriteMicromapsPropertiesEXT
 * [ ] PFN_vkCmdWriteTimestamp
 * [ ] PFN_vkCmdWriteTimestamp2
 * [ ] PFN_vkCmdWriteTimestamp2KHR
 * [ ] PFN_vkCompileDeferredNV
 * [ ] PFN_vkCopyAccelerationStructureKHR
 * [ ] PFN_vkCopyAccelerationStructureToMemoryKHR
 * [ ] PFN_vkCopyImageToImageEXT
 * [ ] PFN_vkCopyImageToMemoryEXT
 * [ ] PFN_vkCopyMemoryToAccelerationStructureKHR
 * [ ] PFN_vkCopyMemoryToImageEXT
 * [ ] PFN_vkCopyMemoryToMicromapEXT
 * [ ] PFN_vkCopyMicromapEXT
 * [ ] PFN_vkCopyMicromapToMemoryEXT
 * [ ] PFN_vkCreateAccelerationStructureKHR
 * [ ] PFN_vkCreateAccelerationStructureNV
 * [ ] PFN_vkCreateBuffer
 * [ ] PFN_vkCreateBufferView
 * [ ] PFN_vkCreateCommandPool
 * [ ] PFN_vkCreateComputePipelines
 * [ ] PFN_vkCreateCudaFunctionNV
 * [ ] PFN_vkCreateCudaModuleNV
 * [ ] PFN_vkCreateCuFunctionNVX
 * [ ] PFN_vkCreateCuModuleNVX
 * [ ] PFN_vkCreateDebugReportCallbackEXT
 * [ ] PFN_vkCreateDebugUtilsMessengerEXT
 * [ ] PFN_vkCreateDeferredOperationKHR
 * [ ] PFN_vkCreateDescriptorPool
 * [ ] PFN_vkCreateDescriptorSetLayout
 * [ ] PFN_vkCreateDescriptorUpdateTemplate
 * [ ] PFN_vkCreateDescriptorUpdateTemplateKHR
 * [ ] PFN_vkCreateDevice
 * [ ] PFN_vkCreateDisplayModeKHR
 * [ ] PFN_vkCreateDisplayPlaneSurfaceKHR
 * [ ] PFN_vkCreateEvent
 * [ ] PFN_vkCreateFence
 * [ ] PFN_vkCreateFramebuffer
 * [ ] PFN_vkCreateGraphicsPipelines
 * [ ] PFN_vkCreateHeadlessSurfaceEXT
 * [ ] PFN_vkCreateImage
 * [ ] PFN_vkCreateImageView
 * [ ] PFN_vkCreateIndirectCommandsLayoutNV
 * [ ] PFN_vkCreateInstance
 * [ ] PFN_vkCreateMicromapEXT
 * [ ] PFN_vkCreateOpticalFlowSessionNV
 * [ ] PFN_vkCreatePipelineCache
 * [ ] PFN_vkCreatePipelineLayout
 * [ ] PFN_vkCreatePrivateDataSlot
 * [ ] PFN_vkCreatePrivateDataSlotEXT
 * [ ] PFN_vkCreateQueryPool
 * [ ] PFN_vkCreateRayTracingPipelinesKHR
 * [ ] PFN_vkCreateRayTracingPipelinesNV
 * [ ] PFN_vkCreateRenderPass
 * [ ] PFN_vkCreateRenderPass2
 * [ ] PFN_vkCreateRenderPass2KHR
 * [ ] PFN_vkCreateSampler
 * [ ] PFN_vkCreateSamplerYcbcrConversion
 * [ ] PFN_vkCreateSamplerYcbcrConversionKHR
 * [ ] PFN_vkCreateSemaphore
 * [ ] PFN_vkCreateShaderModule
 * [ ] PFN_vkCreateShadersEXT
 * [ ] PFN_vkCreateSharedSwapchainsKHR
 * [ ] PFN_vkCreateSwapchainKHR
 * [ ] PFN_vkCreateValidationCacheEXT
 * [ ] PFN_vkCreateVideoSessionKHR
 * [ ] PFN_vkCreateVideoSessionParametersKHR
 * [ ] PFN_vkDebugMarkerSetObjectNameEXT
 * [ ] PFN_vkDebugMarkerSetObjectTagEXT
 * [ ] PFN_vkDebugReportCallbackEXT
 * [ ] PFN_vkDebugReportMessageEXT
 * [ ] PFN_vkDebugUtilsMessengerCallbackEXT
 * [ ] PFN_vkDeferredOperationJoinKHR
 * [ ] PFN_vkDestroyAccelerationStructureKHR
 * [ ] PFN_vkDestroyAccelerationStructureNV
 * [ ] PFN_vkDestroyBuffer
 * [ ] PFN_vkDestroyBufferView
 * [ ] PFN_vkDestroyCommandPool
 * [ ] PFN_vkDestroyCudaFunctionNV
 * [ ] PFN_vkDestroyCudaModuleNV
 * [ ] PFN_vkDestroyCuFunctionNVX
 * [ ] PFN_vkDestroyCuModuleNVX
 * [ ] PFN_vkDestroyDebugReportCallbackEXT
 * [ ] PFN_vkDestroyDebugUtilsMessengerEXT
 * [ ] PFN_vkDestroyDeferredOperationKHR
 * [ ] PFN_vkDestroyDescriptorPool
 * [ ] PFN_vkDestroyDescriptorSetLayout
 * [ ] PFN_vkDestroyDescriptorUpdateTemplate
 * [ ] PFN_vkDestroyDescriptorUpdateTemplateKHR
 * [ ] PFN_vkDestroyDevice
 * [ ] PFN_vkDestroyEvent
 * [ ] PFN_vkDestroyFence
 * [ ] PFN_vkDestroyFramebuffer
 * [ ] PFN_vkDestroyImage
 * [ ] PFN_vkDestroyImageView
 * [ ] PFN_vkDestroyIndirectCommandsLayoutNV
 * [ ] PFN_vkDestroyInstance
 * [ ] PFN_vkDestroyMicromapEXT
 * [ ] PFN_vkDestroyOpticalFlowSessionNV
 * [ ] PFN_vkDestroyPipeline
 * [ ] PFN_vkDestroyPipelineCache
 * [ ] PFN_vkDestroyPipelineLayout
 * [ ] PFN_vkDestroyPrivateDataSlot
 * [ ] PFN_vkDestroyPrivateDataSlotEXT
 * [ ] PFN_vkDestroyQueryPool
 * [ ] PFN_vkDestroyRenderPass
 * [ ] PFN_vkDestroySampler
 * [ ] PFN_vkDestroySamplerYcbcrConversion
 * [ ] PFN_vkDestroySamplerYcbcrConversionKHR
 * [ ] PFN_vkDestroySemaphore
 * [ ] PFN_vkDestroyShaderEXT
 * [ ] PFN_vkDestroyShaderModule
 * [ ] PFN_vkDestroySurfaceKHR
 * [ ] PFN_vkDestroySwapchainKHR
 * [ ] PFN_vkDestroyValidationCacheEXT
 * [ ] PFN_vkDestroyVideoSessionKHR
 * [ ] PFN_vkDestroyVideoSessionParametersKHR
 * [ ] PFN_vkDeviceMemoryReportCallbackEXT
 * [ ] PFN_vkDeviceWaitIdle
 * [ ] PFN_vkDisplayPowerControlEXT
 * [ ] PFN_vkEndCommandBuffer
 * [ ] PFN_vkEnumerateDeviceExtensionProperties
 * [ ] PFN_vkEnumerateDeviceLayerProperties
 * [ ] PFN_vkEnumerateInstanceExtensionProperties
 * [ ] PFN_vkEnumerateInstanceLayerProperties
 * [x] PFN_vkEnumerateInstanceVersion
 * [ ] PFN_vkEnumeratePhysicalDeviceGroups
 * [ ] PFN_vkEnumeratePhysicalDeviceGroupsKHR
 * [ ] PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR
 * [ ] PFN_vkEnumeratePhysicalDevices
 * [ ] PFN_vkFlushMappedMemoryRanges
 * [ ] PFN_vkFreeCommandBuffers
 * [ ] PFN_vkFreeDescriptorSets
 * [ ] PFN_vkFreeFunction
 * [ ] PFN_vkFreeMemory
 * [ ] PFN_vkGetAccelerationStructureBuildSizesKHR
 * [ ] PFN_vkGetAccelerationStructureDeviceAddressKHR
 * [ ] PFN_vkGetAccelerationStructureHandleNV
 * [ ] PFN_vkGetAccelerationStructureMemoryRequirementsNV
 * [ ] PFN_vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT
 * [ ] PFN_vkGetBufferDeviceAddress
 * [ ] PFN_vkGetBufferDeviceAddressEXT
 * [ ] PFN_vkGetBufferDeviceAddressKHR
 * [ ] PFN_vkGetBufferMemoryRequirements
 * [ ] PFN_vkGetBufferMemoryRequirements2
 * [ ] PFN_vkGetBufferMemoryRequirements2KHR
 * [ ] PFN_vkGetBufferOpaqueCaptureAddress
 * [ ] PFN_vkGetBufferOpaqueCaptureAddressKHR
 * [ ] PFN_vkGetBufferOpaqueCaptureDescriptorDataEXT
 * [ ] PFN_vkGetCalibratedTimestampsEXT
 * [ ] PFN_vkGetCalibratedTimestampsKHR
 * [ ] PFN_vkGetCudaModuleCacheNV
 * [ ] PFN_vkGetDeferredOperationMaxConcurrencyKHR
 * [ ] PFN_vkGetDeferredOperationResultKHR
 * [ ] PFN_vkGetDescriptorEXT
 * [ ] PFN_vkGetDescriptorSetHostMappingVALVE
 * [ ] PFN_vkGetDescriptorSetLayoutBindingOffsetEXT
 * [ ] PFN_vkGetDescriptorSetLayoutHostMappingInfoVALVE
 * [ ] PFN_vkGetDescriptorSetLayoutSizeEXT
 * [ ] PFN_vkGetDescriptorSetLayoutSupport
 * [ ] PFN_vkGetDescriptorSetLayoutSupportKHR
 * [ ] PFN_vkGetDeviceAccelerationStructureCompatibilityKHR
 * [ ] PFN_vkGetDeviceBufferMemoryRequirements
 * [ ] PFN_vkGetDeviceBufferMemoryRequirementsKHR
 * [ ] PFN_vkGetDeviceFaultInfoEXT
 * [ ] PFN_vkGetDeviceGroupPeerMemoryFeatures
 * [ ] PFN_vkGetDeviceGroupPeerMemoryFeaturesKHR
 * [ ] PFN_vkGetDeviceGroupPresentCapabilitiesKHR
 * [ ] PFN_vkGetDeviceGroupSurfacePresentModesKHR
 * [ ] PFN_vkGetDeviceImageMemoryRequirements
 * [ ] PFN_vkGetDeviceImageMemoryRequirementsKHR
 * [ ] PFN_vkGetDeviceImageSparseMemoryRequirements
 * [ ] PFN_vkGetDeviceImageSparseMemoryRequirementsKHR
 * [ ] PFN_vkGetDeviceImageSubresourceLayoutKHR
 * [ ] PFN_vkGetDeviceMemoryCommitment
 * [ ] PFN_vkGetDeviceMemoryOpaqueCaptureAddress
 * [ ] PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR
 * [ ] PFN_vkGetDeviceMicromapCompatibilityEXT
 * [ ] PFN_vkGetDeviceProcAddr
 * [ ] PFN_vkGetDeviceQueue
 * [ ] PFN_vkGetDeviceQueue2
 * [ ] PFN_vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI
 * [ ] PFN_vkGetDisplayModeProperties2KHR
 * [ ] PFN_vkGetDisplayModePropertiesKHR
 * [ ] PFN_vkGetDisplayPlaneCapabilities2KHR
 * [ ] PFN_vkGetDisplayPlaneCapabilitiesKHR
 * [ ] PFN_vkGetDisplayPlaneSupportedDisplaysKHR
 * [ ] PFN_vkGetDrmDisplayEXT
 * [ ] PFN_vkGetDynamicRenderingTilePropertiesQCOM
 * [ ] PFN_vkGetEncodedVideoSessionParametersKHR
 * [ ] PFN_vkGetEventStatus
 * [ ] PFN_vkGetFenceFdKHR
 * [ ] PFN_vkGetFenceStatus
 * [ ] PFN_vkGetFramebufferTilePropertiesQCOM
 * [ ] PFN_vkGetGeneratedCommandsMemoryRequirementsNV
 * [ ] PFN_vkGetImageDrmFormatModifierPropertiesEXT
 * [ ] PFN_vkGetImageMemoryRequirements
 * [ ] PFN_vkGetImageMemoryRequirements2
 * [ ] PFN_vkGetImageMemoryRequirements2KHR
 * [ ] PFN_vkGetImageOpaqueCaptureDescriptorDataEXT
 * [ ] PFN_vkGetImageSparseMemoryRequirements
 * [ ] PFN_vkGetImageSparseMemoryRequirements2
 * [ ] PFN_vkGetImageSparseMemoryRequirements2KHR
 * [ ] PFN_vkGetImageSubresourceLayout
 * [ ] PFN_vkGetImageSubresourceLayout2EXT
 * [ ] PFN_vkGetImageSubresourceLayout2KHR
 * [ ] PFN_vkGetImageViewAddressNVX
 * [ ] PFN_vkGetImageViewHandleNVX
 * [ ] PFN_vkGetImageViewOpaqueCaptureDescriptorDataEXT
 * [ ] PFN_vkGetInstanceProcAddr
 * [ ] PFN_vkGetInstanceProcAddrLUNARG
 * [ ] PFN_vkGetLatencyTimingsNV
 * [ ] PFN_vkGetMemoryFdKHR
 * [ ] PFN_vkGetMemoryFdPropertiesKHR
 * [ ] PFN_vkGetMemoryHostPointerPropertiesEXT
 * [ ] PFN_vkGetMemoryRemoteAddressNV
 * [ ] PFN_vkGetMicromapBuildSizesEXT
 * [ ] PFN_vkGetPastPresentationTimingGOOGLE
 * [ ] PFN_vkGetPerformanceParameterINTEL
 * [ ] PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT
 * [ ] PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
 * [ ] PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesNV
 * [ ] PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceDisplayProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceDisplayPropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceExternalBufferProperties
 * [ ] PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceExternalFenceProperties
 * [ ] PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceExternalImageFormatPropertiesNV
 * [ ] PFN_vkGetPhysicalDeviceExternalSemaphoreProperties
 * [ ] PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceFeatures
 * [ ] PFN_vkGetPhysicalDeviceFeatures2
 * [ ] PFN_vkGetPhysicalDeviceFeatures2KHR
 * [ ] PFN_vkGetPhysicalDeviceFormatProperties
 * [ ] PFN_vkGetPhysicalDeviceFormatProperties2
 * [ ] PFN_vkGetPhysicalDeviceFormatProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR
 * [ ] PFN_vkGetPhysicalDeviceImageFormatProperties
 * [ ] PFN_vkGetPhysicalDeviceImageFormatProperties2
 * [ ] PFN_vkGetPhysicalDeviceImageFormatProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceMemoryProperties
 * [ ] PFN_vkGetPhysicalDeviceMemoryProperties2
 * [ ] PFN_vkGetPhysicalDeviceMemoryProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceMultisamplePropertiesEXT
 * [ ] PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV
 * [ ] PFN_vkGetPhysicalDevicePresentRectanglesKHR
 * [ ] PFN_vkGetPhysicalDeviceProperties
 * [ ] PFN_vkGetPhysicalDeviceProperties2
 * [ ] PFN_vkGetPhysicalDeviceProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR
 * [ ] PFN_vkGetPhysicalDeviceQueueFamilyProperties
 * [ ] PFN_vkGetPhysicalDeviceQueueFamilyProperties2
 * [ ] PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceSparseImageFormatProperties
 * [ ] PFN_vkGetPhysicalDeviceSparseImageFormatProperties2
 * [ ] PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR
 * [ ] PFN_vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV
 * [ ] PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT
 * [ ] PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR
 * [ ] PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
 * [ ] PFN_vkGetPhysicalDeviceSurfaceFormats2KHR
 * [ ] PFN_vkGetPhysicalDeviceSurfaceFormatsKHR
 * [ ] PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
 * [ ] PFN_vkGetPhysicalDeviceSurfaceSupportKHR
 * [ ] PFN_vkGetPhysicalDeviceToolProperties
 * [ ] PFN_vkGetPhysicalDeviceToolPropertiesEXT
 * [ ] PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR
 * [ ] PFN_vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR
 * [ ] PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR
 * [ ] PFN_vkGetPipelineCacheData
 * [ ] PFN_vkGetPipelineExecutableInternalRepresentationsKHR
 * [ ] PFN_vkGetPipelineExecutablePropertiesKHR
 * [ ] PFN_vkGetPipelineExecutableStatisticsKHR
 * [ ] PFN_vkGetPipelineIndirectDeviceAddressNV
 * [ ] PFN_vkGetPipelineIndirectMemoryRequirementsNV
 * [ ] PFN_vkGetPipelinePropertiesEXT
 * [ ] PFN_vkGetPrivateData
 * [ ] PFN_vkGetPrivateDataEXT
 * [ ] PFN_vkGetQueryPoolResults
 * [ ] PFN_vkGetQueueCheckpointData2NV
 * [ ] PFN_vkGetQueueCheckpointDataNV
 * [ ] PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR
 * [ ] PFN_vkGetRayTracingShaderGroupHandlesKHR
 * [ ] PFN_vkGetRayTracingShaderGroupHandlesNV
 * [ ] PFN_vkGetRayTracingShaderGroupStackSizeKHR
 * [ ] PFN_vkGetRefreshCycleDurationGOOGLE
 * [ ] PFN_vkGetRenderAreaGranularity
 * [ ] PFN_vkGetRenderingAreaGranularityKHR
 * [ ] PFN_vkGetSamplerOpaqueCaptureDescriptorDataEXT
 * [ ] PFN_vkGetSemaphoreCounterValue
 * [ ] PFN_vkGetSemaphoreCounterValueKHR
 * [ ] PFN_vkGetSemaphoreFdKHR
 * [ ] PFN_vkGetShaderBinaryDataEXT
 * [ ] PFN_vkGetShaderInfoAMD
 * [ ] PFN_vkGetShaderModuleCreateInfoIdentifierEXT
 * [ ] PFN_vkGetShaderModuleIdentifierEXT
 * [ ] PFN_vkGetSwapchainCounterEXT
 * [ ] PFN_vkGetSwapchainImagesKHR
 * [ ] PFN_vkGetSwapchainStatusKHR
 * [ ] PFN_vkGetValidationCacheDataEXT
 * [ ] PFN_vkGetVideoSessionMemoryRequirementsKHR
 * [ ] PFN_vkImportFenceFdKHR
 * [ ] PFN_vkImportSemaphoreFdKHR
 * [ ] PFN_vkInitializePerformanceApiINTEL
 * [ ] PFN_vkInternalAllocationNotification
 * [ ] PFN_vkInternalFreeNotification
 * [ ] PFN_vkInvalidateMappedMemoryRanges
 * [ ] PFN_vkLatencySleepNV
 * [ ] PFN_vkMapMemory
 * [ ] PFN_vkMapMemory2KHR
 * [ ] PFN_vkMergePipelineCaches
 * [ ] PFN_vkMergeValidationCachesEXT
 * [ ] PFN_vkQueueBeginDebugUtilsLabelEXT
 * [ ] PFN_vkQueueBindSparse
 * [ ] PFN_vkQueueEndDebugUtilsLabelEXT
 * [ ] PFN_vkQueueInsertDebugUtilsLabelEXT
 * [ ] PFN_vkQueueNotifyOutOfBandNV
 * [ ] PFN_vkQueuePresentKHR
 * [ ] PFN_vkQueueSetPerformanceConfigurationINTEL
 * [ ] PFN_vkQueueSubmit
 * [ ] PFN_vkQueueSubmit2
 * [ ] PFN_vkQueueSubmit2KHR
 * [ ] PFN_vkQueueWaitIdle
 * [ ] PFN_vkReallocationFunction
 * [ ] PFN_vkRegisterDeviceEventEXT
 * [ ] PFN_vkRegisterDisplayEventEXT
 * [ ] PFN_vkReleaseDisplayEXT
 * [ ] PFN_vkReleasePerformanceConfigurationINTEL
 * [ ] PFN_vkReleaseProfilingLockKHR
 * [ ] PFN_vkReleaseSwapchainImagesEXT
 * [ ] PFN_vkResetCommandBuffer
 * [ ] PFN_vkResetCommandPool
 * [ ] PFN_vkResetDescriptorPool
 * [ ] PFN_vkResetEvent
 * [ ] PFN_vkResetFences
 * [ ] PFN_vkResetQueryPool
 * [ ] PFN_vkResetQueryPoolEXT
 * [ ] PFN_vkSetDebugUtilsObjectNameEXT
 * [ ] PFN_vkSetDebugUtilsObjectTagEXT
 * [ ] PFN_vkSetDeviceMemoryPriorityEXT
 * [ ] PFN_vkSetEvent
 * [ ] PFN_vkSetHdrMetadataEXT
 * [ ] PFN_vkSetLatencyMarkerNV
 * [ ] PFN_vkSetLatencySleepModeNV
 * [ ] PFN_vkSetLocalDimmingAMD
 * [ ] PFN_vkSetPrivateData
 * [ ] PFN_vkSetPrivateDataEXT
 * [ ] PFN_vkSignalSemaphore
 * [ ] PFN_vkSignalSemaphoreKHR
 * [ ] PFN_vkSubmitDebugUtilsMessageEXT
 * [ ] PFN_vkTransitionImageLayoutEXT
 * [ ] PFN_vkTrimCommandPool
 * [ ] PFN_vkTrimCommandPoolKHR
 * [ ] PFN_vkUninitializePerformanceApiINTEL
 * [ ] PFN_vkUnmapMemory
 * [ ] PFN_vkUnmapMemory2KHR
 * [ ] PFN_vkUpdateDescriptorSets
 * [ ] PFN_vkUpdateDescriptorSetWithTemplate
 * [ ] PFN_vkUpdateDescriptorSetWithTemplateKHR
 * [ ] PFN_vkUpdateVideoSessionParametersKHR
 * [ ] PFN_vkVoidFunction
 * [ ] PFN_vkWaitForFences
 * [ ] PFN_vkWaitForPresentKHR
 * [ ] PFN_vkWaitSemaphores
 * [ ] PFN_vkWaitSemaphoresKHR
 * [ ] PFN_vkWriteAccelerationStructuresPropertiesKHR
 * [ ] PFN_vkWriteMicromapsPropertiesEXT
 * [ ] VkAabbPositionsKHR
 * [ ] VkAabbPositionsNV
 * [ ] VkAccelerationStructureBuildGeometryInfoKHR
 * [ ] VkAccelerationStructureBuildRangeInfoKHR
 * [ ] VkAccelerationStructureBuildSizesInfoKHR
 * [ ] VkAccelerationStructureBuildTypeKHR
 * [ ] VkAccelerationStructureCaptureDescriptorDataInfoEXT
 * [ ] VkAccelerationStructureCompatibilityKHR
 * [ ] VkAccelerationStructureCreateFlagBitsKHR
 * [ ] VkAccelerationStructureCreateFlagsKHR
 * [ ] VkAccelerationStructureCreateInfoKHR
 * [ ] VkAccelerationStructureCreateInfoNV
 * [ ] VkAccelerationStructureDeviceAddressInfoKHR
 * [ ] VkAccelerationStructureGeometryAabbsDataKHR
 * [ ] VkAccelerationStructureGeometryDataKHR
 * [ ] VkAccelerationStructureGeometryInstancesDataKHR
 * [ ] VkAccelerationStructureGeometryKHR
 * [ ] VkAccelerationStructureGeometryMotionTrianglesDataNV
 * [ ] VkAccelerationStructureGeometryTrianglesDataKHR
 * [ ] VkAccelerationStructureInfoNV
 * [ ] VkAccelerationStructureInstanceKHR
 * [ ] VkAccelerationStructureInstanceNV
 * [ ] VkAccelerationStructureKHR
 * [ ] VkAccelerationStructureMatrixMotionInstanceNV
 * [ ] VkAccelerationStructureMemoryRequirementsInfoNV
 * [ ] VkAccelerationStructureMemoryRequirementsTypeNV
 * [ ] VkAccelerationStructureMotionInfoFlagsNV
 * [ ] VkAccelerationStructureMotionInfoNV
 * [ ] VkAccelerationStructureMotionInstanceDataNV
 * [ ] VkAccelerationStructureMotionInstanceFlagsNV
 * [ ] VkAccelerationStructureMotionInstanceNV
 * [ ] VkAccelerationStructureMotionInstanceTypeNV
 * [ ] VkAccelerationStructureNV
 * [ ] VkAccelerationStructureSRTMotionInstanceNV
 * [ ] VkAccelerationStructureTrianglesOpacityMicromapEXT
 * [ ] VkAccelerationStructureTypeKHR
 * [ ] VkAccelerationStructureTypeNV
 * [ ] VkAccelerationStructureVersionInfoKHR
 * [ ] VkAccessFlagBits
 * [ ] VkAccessFlagBits2
 * [ ] VkAccessFlagBits2KHR
 * [ ] VkAccessFlags
 * [ ] VkAccessFlags2
 * [ ] VkAccessFlags2KHR
 * [ ] VkAcquireNextImageInfoKHR
 * [ ] VkAcquireProfilingLockFlagBitsKHR
 * [ ] VkAcquireProfilingLockFlagsKHR
 * [ ] VkAcquireProfilingLockInfoKHR
 * [x] VkAllocationCallbacks
 * [ ] VkAmigoProfilingSubmitInfoSEC
 * [x] VkApplicationInfo
 * [x] VkAttachmentDescription
 * [ ] VkAttachmentDescription2
 * [ ] VkAttachmentDescription2KHR
 * [ ] VkAttachmentDescriptionFlagBits
 * [ ] VkAttachmentDescriptionFlags
 * [ ] VkAttachmentDescriptionStencilLayout
 * [ ] VkAttachmentDescriptionStencilLayoutKHR
 * [ ] VkAttachmentLoadOp
 * [x] VkAttachmentReference
 * [ ] VkAttachmentReference2
 * [ ] VkAttachmentReference2KHR
 * [ ] VkAttachmentReferenceStencilLayout
 * [ ] VkAttachmentReferenceStencilLayoutKHR
 * [ ] VkAttachmentSampleCountInfoAMD
 * [ ] VkAttachmentSampleCountInfoNV
 * [ ] VkAttachmentSampleLocationsEXT
 * [ ] VkAttachmentStoreOp
 * [ ] VkBaseInStructure
 * [ ] VkBaseOutStructure
 * [ ] VkBindAccelerationStructureMemoryInfoNV
 * [ ] VkBindBufferMemoryDeviceGroupInfo
 * [ ] VkBindBufferMemoryDeviceGroupInfoKHR
 * [ ] VkBindBufferMemoryInfo
 * [ ] VkBindBufferMemoryInfoKHR
 * [ ] VkBindDescriptorBufferEmbeddedSamplersInfoEXT
 * [ ] VkBindDescriptorSetsInfoKHR
 * [ ] VkBindImageMemoryDeviceGroupInfo
 * [ ] VkBindImageMemoryDeviceGroupInfoKHR
 * [ ] VkBindImageMemoryInfo
 * [ ] VkBindImageMemoryInfoKHR
 * [ ] VkBindImageMemorySwapchainInfoKHR
 * [ ] VkBindImagePlaneMemoryInfo
 * [ ] VkBindImagePlaneMemoryInfoKHR
 * [ ] VkBindIndexBufferIndirectCommandNV
 * [ ] VkBindMemoryStatusKHR
 * [ ] VkBindPipelineIndirectCommandNV
 * [ ] VkBindShaderGroupIndirectCommandNV
 * [ ] VkBindSparseInfo
 * [ ] VkBindVertexBufferIndirectCommandNV
 * [ ] VkBindVideoSessionMemoryInfoKHR
 * [ ] VkBlendFactor
 * [ ] VkBlendOp
 * [ ] VkBlendOverlapEXT
 * [ ] VkBlitImageCubicWeightsInfoQCOM
 * [ ] VkBlitImageInfo2
 * [ ] VkBlitImageInfo2KHR
 * [ ] VkBlockMatchWindowCompareModeQCOM
 * [x] VkBool32
 * [ ] VkBorderColor
 * [x] VkBuffer
 * [ ] VkBufferCaptureDescriptorDataInfoEXT
 * [x] VkBufferCopy
 * [ ] VkBufferCopy2
 * [ ] VkBufferCopy2KHR
 * [ ] VkBufferCreateFlagBits
 * [ ] VkBufferCreateFlags
 * [x] VkBufferCreateInfo
 * [ ] VkBufferDeviceAddressCreateInfoEXT
 * [ ] VkBufferDeviceAddressInfo
 * [ ] VkBufferDeviceAddressInfoEXT
 * [ ] VkBufferDeviceAddressInfoKHR
 * [ ] VkBufferImageCopy
 * [ ] VkBufferImageCopy2
 * [ ] VkBufferImageCopy2KHR
 * [ ] VkBufferMemoryBarrier
 * [ ] VkBufferMemoryBarrier2
 * [ ] VkBufferMemoryBarrier2KHR
 * [ ] VkBufferMemoryRequirementsInfo2
 * [ ] VkBufferMemoryRequirementsInfo2KHR
 * [ ] VkBufferOpaqueCaptureAddressCreateInfo
 * [ ] VkBufferOpaqueCaptureAddressCreateInfoKHR
 * [ ] VkBufferUsageFlagBits
 * [ ] VkBufferUsageFlagBits2KHR
 * [x] VkBufferUsageFlags
 * [ ] VkBufferUsageFlags2CreateInfoKHR
 * [ ] VkBufferUsageFlags2KHR
 * [ ] VkBufferView
 * [ ] VkBufferViewCreateFlags
 * [ ] VkBufferViewCreateInfo
 * [ ] VkBuildAccelerationStructureFlagBitsKHR
 * [ ] VkBuildAccelerationStructureFlagBitsNV
 * [ ] VkBuildAccelerationStructureFlagsKHR
 * [ ] VkBuildAccelerationStructureFlagsNV
 * [ ] VkBuildAccelerationStructureModeKHR
 * [ ] VkBuildMicromapFlagBitsEXT
 * [ ] VkBuildMicromapFlagsEXT
 * [ ] VkBuildMicromapModeEXT
 * [ ] VkCalibratedTimestampInfoEXT
 * [ ] VkCalibratedTimestampInfoKHR
 * [ ] VkCheckpointData2NV
 * [ ] VkCheckpointDataNV
 * [ ] VkChromaLocation
 * [ ] VkChromaLocationKHR
 * [ ] VkClearAttachment
 * [ ] VkClearColorValue
 * [ ] VkClearDepthStencilValue
 * [ ] VkClearRect
 * [x] VkClearValue
 * [ ] VkCoarseSampleLocationNV
 * [ ] VkCoarseSampleOrderCustomNV
 * [ ] VkCoarseSampleOrderTypeNV
 * [ ] VkColorBlendAdvancedEXT
 * [ ] VkColorBlendEquationEXT
 * [ ] VkColorComponentFlagBits
 * [ ] VkColorComponentFlags
 * [ ] VkColorSpaceKHR
 * [x] VkCommandBuffer
 * [x] VkCommandBufferAllocateInfo
 * [x] VkCommandBufferBeginInfo
 * [ ] VkCommandBufferInheritanceConditionalRenderingInfoEXT
 * [ ] VkCommandBufferInheritanceInfo
 * [ ] VkCommandBufferInheritanceRenderingInfo
 * [ ] VkCommandBufferInheritanceRenderingInfoKHR
 * [ ] VkCommandBufferInheritanceRenderPassTransformInfoQCOM
 * [ ] VkCommandBufferInheritanceViewportScissorInfoNV
 * [ ] VkCommandBufferLevel
 * [ ] VkCommandBufferResetFlagBits
 * [ ] VkCommandBufferResetFlags
 * [ ] VkCommandBufferSubmitInfo
 * [ ] VkCommandBufferSubmitInfoKHR
 * [x] VkCommandBufferUsageFlagBits
 * [ ] VkCommandBufferUsageFlags
 * [x] VkCommandPool
 * [ ] VkCommandPoolCreateFlagBits
 * [ ] VkCommandPoolCreateFlags
 * [x] VkCommandPoolCreateInfo
 * [ ] VkCommandPoolResetFlagBits
 * [ ] VkCommandPoolResetFlags
 * [ ] VkCommandPoolTrimFlags
 * [ ] VkCommandPoolTrimFlagsKHR
 * [ ] VkCompareOp
 * [ ] VkComponentMapping
 * [ ] VkComponentSwizzle
 * [ ] VkComponentTypeKHR
 * [ ] VkComponentTypeNV
 * [ ] VkCompositeAlphaFlagBitsKHR
 * [ ] VkCompositeAlphaFlagsKHR
 * [ ] VkComputePipelineCreateInfo
 * [ ] VkComputePipelineIndirectBufferInfoNV
 * [ ] VkConditionalRenderingBeginInfoEXT
 * [ ] VkConditionalRenderingFlagBitsEXT
 * [ ] VkConditionalRenderingFlagsEXT
 * [ ] VkConformanceVersion
 * [ ] VkConformanceVersionKHR
 * [ ] VkConservativeRasterizationModeEXT
 * [ ] VkCooperativeMatrixPropertiesKHR
 * [ ] VkCooperativeMatrixPropertiesNV
 * [ ] VkCopyAccelerationStructureInfoKHR
 * [ ] VkCopyAccelerationStructureModeKHR
 * [ ] VkCopyAccelerationStructureModeNV
 * [ ] VkCopyAccelerationStructureToMemoryInfoKHR
 * [ ] VkCopyBufferInfo2
 * [ ] VkCopyBufferInfo2KHR
 * [ ] VkCopyBufferToImageInfo2
 * [ ] VkCopyBufferToImageInfo2KHR
 * [ ] VkCopyCommandTransformInfoQCOM
 * [ ] VkCopyDescriptorSet
 * [ ] VkCopyImageInfo2
 * [ ] VkCopyImageInfo2KHR
 * [ ] VkCopyImageToBufferInfo2
 * [ ] VkCopyImageToBufferInfo2KHR
 * [ ] VkCopyImageToImageInfoEXT
 * [ ] VkCopyImageToMemoryInfoEXT
 * [ ] VkCopyMemoryIndirectCommandNV
 * [ ] VkCopyMemoryToAccelerationStructureInfoKHR
 * [ ] VkCopyMemoryToImageIndirectCommandNV
 * [ ] VkCopyMemoryToImageInfoEXT
 * [ ] VkCopyMemoryToMicromapInfoEXT
 * [ ] VkCopyMicromapInfoEXT
 * [ ] VkCopyMicromapModeEXT
 * [ ] VkCopyMicromapToMemoryInfoEXT
 * [ ] VkCoverageModulationModeNV
 * [ ] VkCoverageReductionModeNV
 * [ ] VkCubicFilterWeightsQCOM
 * [ ] VkCudaFunctionCreateInfoNV
 * [ ] VkCudaFunctionNV
 * [ ] VkCudaLaunchInfoNV
 * [ ] VkCudaModuleCreateInfoNV
 * [ ] VkCudaModuleNV
 * [ ] VkCuFunctionCreateInfoNVX
 * [ ] VkCuFunctionNVX
 * [ ] VkCuLaunchInfoNVX
 * [ ] VkCullModeFlagBits
 * [x] VkCullModeFlags
 * [ ] VkCuModuleCreateInfoNVX
 * [ ] VkCuModuleNVX
 * [ ] VkDebugMarkerMarkerInfoEXT
 * [ ] VkDebugMarkerObjectNameInfoEXT
 * [ ] VkDebugMarkerObjectTagInfoEXT
 * [ ] VkDebugReportCallbackCreateInfoEXT
 * [ ] VkDebugReportCallbackEXT
 * [ ] VkDebugReportFlagBitsEXT
 * [ ] VkDebugReportFlagsEXT
 * [ ] VkDebugReportObjectTypeEXT
 * [ ] VkDebugUtilsLabelEXT
 * [ ] VkDebugUtilsMessageSeverityFlagBitsEXT
 * [ ] VkDebugUtilsMessageSeverityFlagsEXT
 * [ ] VkDebugUtilsMessageTypeFlagBitsEXT
 * [ ] VkDebugUtilsMessageTypeFlagsEXT
 * [ ] VkDebugUtilsMessengerCallbackDataEXT
 * [ ] VkDebugUtilsMessengerCallbackDataFlagsEXT
 * [ ] VkDebugUtilsMessengerCreateFlagsEXT
 * [ ] VkDebugUtilsMessengerCreateInfoEXT
 * [ ] VkDebugUtilsMessengerEXT
 * [ ] VkDebugUtilsObjectNameInfoEXT
 * [ ] VkDebugUtilsObjectTagInfoEXT
 * [ ] VkDecompressMemoryRegionNV
 * [ ] VkDedicatedAllocationBufferCreateInfoNV
 * [ ] VkDedicatedAllocationImageCreateInfoNV
 * [ ] VkDedicatedAllocationMemoryAllocateInfoNV
 * [ ] VkDeferredOperationKHR
 * [ ] VkDependencyFlagBits
 * [ ] VkDependencyFlags
 * [ ] VkDependencyInfo
 * [ ] VkDependencyInfoKHR
 * [ ] VkDepthBiasInfoEXT
 * [ ] VkDepthBiasRepresentationEXT
 * [ ] VkDepthBiasRepresentationInfoEXT
 * [ ] VkDescriptorAddressInfoEXT
 * [ ] VkDescriptorBindingFlagBits
 * [ ] VkDescriptorBindingFlagBitsEXT
 * [ ] VkDescriptorBindingFlags
 * [ ] VkDescriptorBindingFlagsEXT
 * [ ] VkDescriptorBufferBindingInfoEXT
 * [ ] VkDescriptorBufferBindingPushDescriptorBufferHandleEXT
 * [ ] VkDescriptorBufferInfo
 * [ ] VkDescriptorDataEXT
 * [ ] VkDescriptorGetInfoEXT
 * [ ] VkDescriptorImageInfo
 * [ ] VkDescriptorPool
 * [ ] VkDescriptorPoolCreateFlagBits
 * [ ] VkDescriptorPoolCreateFlags
 * [ ] VkDescriptorPoolCreateInfo
 * [ ] VkDescriptorPoolInlineUniformBlockCreateInfo
 * [ ] VkDescriptorPoolInlineUniformBlockCreateInfoEXT
 * [ ] VkDescriptorPoolResetFlags
 * [ ] VkDescriptorPoolSize
 * [ ] VkDescriptorSet
 * [ ] VkDescriptorSetAllocateInfo
 * [ ] VkDescriptorSetBindingReferenceVALVE
 * [ ] VkDescriptorSetLayout
 * [ ] VkDescriptorSetLayoutBinding
 * [ ] VkDescriptorSetLayoutBindingFlagsCreateInfo
 * [ ] VkDescriptorSetLayoutBindingFlagsCreateInfoEXT
 * [ ] VkDescriptorSetLayoutCreateFlagBits
 * [ ] VkDescriptorSetLayoutCreateFlags
 * [ ] VkDescriptorSetLayoutCreateInfo
 * [ ] VkDescriptorSetLayoutHostMappingInfoVALVE
 * [ ] VkDescriptorSetLayoutSupport
 * [ ] VkDescriptorSetLayoutSupportKHR
 * [ ] VkDescriptorSetVariableDescriptorCountAllocateInfo
 * [ ] VkDescriptorSetVariableDescriptorCountAllocateInfoEXT
 * [ ] VkDescriptorSetVariableDescriptorCountLayoutSupport
 * [ ] VkDescriptorSetVariableDescriptorCountLayoutSupportEXT
 * [ ] VkDescriptorType
 * [ ] VkDescriptorUpdateTemplate
 * [ ] VkDescriptorUpdateTemplateCreateFlags
 * [ ] VkDescriptorUpdateTemplateCreateFlagsKHR
 * [ ] VkDescriptorUpdateTemplateCreateInfo
 * [ ] VkDescriptorUpdateTemplateCreateInfoKHR
 * [ ] VkDescriptorUpdateTemplateEntry
 * [ ] VkDescriptorUpdateTemplateEntryKHR
 * [ ] VkDescriptorUpdateTemplateKHR
 * [ ] VkDescriptorUpdateTemplateType
 * [ ] VkDescriptorUpdateTemplateTypeKHR
 * [x] VkDevice
 * [ ] VkDeviceAddress
 * [ ] VkDeviceAddressBindingCallbackDataEXT
 * [ ] VkDeviceAddressBindingFlagBitsEXT
 * [ ] VkDeviceAddressBindingFlagsEXT
 * [ ] VkDeviceAddressBindingTypeEXT
 * [ ] VkDeviceBufferMemoryRequirements
 * [ ] VkDeviceBufferMemoryRequirementsKHR
 * [ ] VkDeviceCreateFlags
 * [x] VkDeviceCreateInfo
 * [ ] VkDeviceDeviceMemoryReportCreateInfoEXT
 * [ ] VkDeviceDiagnosticsConfigCreateInfoNV
 * [ ] VkDeviceDiagnosticsConfigFlagBitsNV
 * [ ] VkDeviceDiagnosticsConfigFlagsNV
 * [ ] VkDeviceEventInfoEXT
 * [ ] VkDeviceEventTypeEXT
 * [ ] VkDeviceFaultAddressInfoEXT
 * [ ] VkDeviceFaultAddressTypeEXT
 * [ ] VkDeviceFaultCountsEXT
 * [ ] VkDeviceFaultInfoEXT
 * [ ] VkDeviceFaultVendorBinaryHeaderVersionEXT
 * [ ] VkDeviceFaultVendorBinaryHeaderVersionOneEXT
 * [ ] VkDeviceFaultVendorInfoEXT
 * [ ] VkDeviceGroupBindSparseInfo
 * [ ] VkDeviceGroupBindSparseInfoKHR
 * [ ] VkDeviceGroupCommandBufferBeginInfo
 * [ ] VkDeviceGroupCommandBufferBeginInfoKHR
 * [ ] VkDeviceGroupDeviceCreateInfo
 * [ ] VkDeviceGroupDeviceCreateInfoKHR
 * [ ] VkDeviceGroupPresentCapabilitiesKHR
 * [ ] VkDeviceGroupPresentInfoKHR
 * [ ] VkDeviceGroupPresentModeFlagBitsKHR
 * [ ] VkDeviceGroupPresentModeFlagsKHR
 * [ ] VkDeviceGroupRenderPassBeginInfo
 * [ ] VkDeviceGroupRenderPassBeginInfoKHR
 * [ ] VkDeviceGroupSubmitInfo
 * [ ] VkDeviceGroupSubmitInfoKHR
 * [ ] VkDeviceGroupSwapchainCreateInfoKHR
 * [ ] VkDeviceImageMemoryRequirements
 * [ ] VkDeviceImageMemoryRequirementsKHR
 * [ ] VkDeviceImageSubresourceInfoKHR
 * [x] VkDeviceMemory
 * [ ] VkDeviceMemoryOpaqueCaptureAddressInfo
 * [ ] VkDeviceMemoryOpaqueCaptureAddressInfoKHR
 * [ ] VkDeviceMemoryOverallocationCreateInfoAMD
 * [ ] VkDeviceMemoryReportCallbackDataEXT
 * [ ] VkDeviceMemoryReportEventTypeEXT
 * [ ] VkDeviceMemoryReportFlagsEXT
 * [ ] VkDeviceOrHostAddressConstKHR
 * [ ] VkDeviceOrHostAddressKHR
 * [ ] VkDevicePrivateDataCreateInfo
 * [ ] VkDevicePrivateDataCreateInfoEXT
 * [ ] VkDeviceQueueCreateFlagBits
 * [ ] VkDeviceQueueCreateFlags
 * [x] VkDeviceQueueCreateInfo
 * [ ] VkDeviceQueueGlobalPriorityCreateInfoEXT
 * [ ] VkDeviceQueueGlobalPriorityCreateInfoKHR
 * [ ] VkDeviceQueueInfo2
 * [ ] VkDeviceQueueShaderCoreControlCreateInfoARM
 * [x] VkDeviceSize
 * [ ] VkDirectDriverLoadingFlagsLUNARG
 * [ ] VkDirectDriverLoadingInfoLUNARG
 * [ ] VkDirectDriverLoadingListLUNARG
 * [ ] VkDirectDriverLoadingModeLUNARG
 * [ ] VkDiscardRectangleModeEXT
 * [ ] VkDispatchIndirectCommand
 * [ ] VkDisplayEventInfoEXT
 * [ ] VkDisplayEventTypeEXT
 * [ ] VkDisplayKHR
 * [ ] VkDisplayModeCreateFlagsKHR
 * [ ] VkDisplayModeCreateInfoKHR
 * [ ] VkDisplayModeKHR
 * [ ] VkDisplayModeParametersKHR
 * [ ] VkDisplayModeProperties2KHR
 * [ ] VkDisplayModePropertiesKHR
 * [ ] VkDisplayNativeHdrSurfaceCapabilitiesAMD
 * [ ] VkDisplayPlaneAlphaFlagBitsKHR
 * [ ] VkDisplayPlaneAlphaFlagsKHR
 * [ ] VkDisplayPlaneCapabilities2KHR
 * [ ] VkDisplayPlaneCapabilitiesKHR
 * [ ] VkDisplayPlaneInfo2KHR
 * [ ] VkDisplayPlaneProperties2KHR
 * [ ] VkDisplayPlanePropertiesKHR
 * [ ] VkDisplayPowerInfoEXT
 * [ ] VkDisplayPowerStateEXT
 * [ ] VkDisplayPresentInfoKHR
 * [ ] VkDisplayProperties2KHR
 * [ ] VkDisplayPropertiesKHR
 * [ ] VkDisplaySurfaceCreateFlagsKHR
 * [ ] VkDisplaySurfaceCreateInfoKHR
 * [ ] VkDrawIndexedIndirectCommand
 * [ ] VkDrawIndirectCommand
 * [ ] VkDrawMeshTasksIndirectCommandEXT
 * [ ] VkDrawMeshTasksIndirectCommandNV
 * [ ] VkDriverId
 * [ ] VkDriverIdKHR
 * [ ] VkDrmFormatModifierProperties2EXT
 * [ ] VkDrmFormatModifierPropertiesEXT
 * [ ] VkDrmFormatModifierPropertiesList2EXT
 * [ ] VkDrmFormatModifierPropertiesListEXT
 * [x] VkDynamicState
 * [ ] VkEvent
 * [ ] VkEventCreateFlagBits
 * [ ] VkEventCreateFlags
 * [ ] VkEventCreateInfo
 * [ ] VkExportFenceCreateInfo
 * [ ] VkExportFenceCreateInfoKHR
 * [ ] VkExportMemoryAllocateInfo
 * [ ] VkExportMemoryAllocateInfoKHR
 * [ ] VkExportMemoryAllocateInfoNV
 * [ ] VkExportSemaphoreCreateInfo
 * [ ] VkExportSemaphoreCreateInfoKHR
 * [ ] VkExtensionProperties
 * [x] VkExtent2D
 * [ ] VkExtent3D
 * [ ] VkExternalBufferProperties
 * [ ] VkExternalBufferPropertiesKHR
 * [ ] VkExternalFenceFeatureFlagBits
 * [ ] VkExternalFenceFeatureFlagBitsKHR
 * [ ] VkExternalFenceFeatureFlags
 * [ ] VkExternalFenceFeatureFlagsKHR
 * [ ] VkExternalFenceHandleTypeFlagBits
 * [ ] VkExternalFenceHandleTypeFlagBitsKHR
 * [ ] VkExternalFenceHandleTypeFlags
 * [ ] VkExternalFenceHandleTypeFlagsKHR
 * [ ] VkExternalFenceProperties
 * [ ] VkExternalFencePropertiesKHR
 * [ ] VkExternalImageFormatProperties
 * [ ] VkExternalImageFormatPropertiesKHR
 * [ ] VkExternalImageFormatPropertiesNV
 * [ ] VkExternalMemoryAcquireUnmodifiedEXT
 * [ ] VkExternalMemoryBufferCreateInfo
 * [ ] VkExternalMemoryBufferCreateInfoKHR
 * [ ] VkExternalMemoryFeatureFlagBits
 * [ ] VkExternalMemoryFeatureFlagBitsKHR
 * [ ] VkExternalMemoryFeatureFlagBitsNV
 * [ ] VkExternalMemoryFeatureFlags
 * [ ] VkExternalMemoryFeatureFlagsKHR
 * [ ] VkExternalMemoryFeatureFlagsNV
 * [ ] VkExternalMemoryHandleTypeFlagBits
 * [ ] VkExternalMemoryHandleTypeFlagBitsKHR
 * [ ] VkExternalMemoryHandleTypeFlagBitsNV
 * [ ] VkExternalMemoryHandleTypeFlags
 * [ ] VkExternalMemoryHandleTypeFlagsKHR
 * [ ] VkExternalMemoryHandleTypeFlagsNV
 * [ ] VkExternalMemoryImageCreateInfo
 * [ ] VkExternalMemoryImageCreateInfoKHR
 * [ ] VkExternalMemoryImageCreateInfoNV
 * [ ] VkExternalMemoryProperties
 * [ ] VkExternalMemoryPropertiesKHR
 * [ ] VkExternalSemaphoreFeatureFlagBits
 * [ ] VkExternalSemaphoreFeatureFlagBitsKHR
 * [ ] VkExternalSemaphoreFeatureFlags
 * [ ] VkExternalSemaphoreFeatureFlagsKHR
 * [ ] VkExternalSemaphoreHandleTypeFlagBits
 * [ ] VkExternalSemaphoreHandleTypeFlagBitsKHR
 * [ ] VkExternalSemaphoreHandleTypeFlags
 * [ ] VkExternalSemaphoreHandleTypeFlagsKHR
 * [ ] VkExternalSemaphoreProperties
 * [ ] VkExternalSemaphorePropertiesKHR
 * [x] VkFence
 * [ ] VkFenceCreateFlagBits
 * [ ] VkFenceCreateFlags
 * [x] VkFenceCreateInfo
 * [ ] VkFenceGetFdInfoKHR
 * [ ] VkFenceImportFlagBits
 * [ ] VkFenceImportFlagBitsKHR
 * [ ] VkFenceImportFlags
 * [ ] VkFenceImportFlagsKHR
 * [ ] VkFilter
 * [ ] VkFilterCubicImageViewImageFormatPropertiesEXT
 * [ ] VkFlags
 * [ ] VkFlags64
 * [x] VkFormat
 * [ ] VkFormatFeatureFlagBits
 * [ ] VkFormatFeatureFlagBits2
 * [ ] VkFormatFeatureFlagBits2KHR
 * [ ] VkFormatFeatureFlags
 * [ ] VkFormatFeatureFlags2
 * [ ] VkFormatFeatureFlags2KHR
 * [x] VkFormatProperties
 * [ ] VkFormatProperties2
 * [ ] VkFormatProperties2KHR
 * [ ] VkFormatProperties3
 * [ ] VkFormatProperties3KHR
 * [ ] VkFragmentShadingRateAttachmentInfoKHR
 * [ ] VkFragmentShadingRateCombinerOpKHR
 * [ ] VkFragmentShadingRateNV
 * [ ] VkFragmentShadingRateTypeNV
 * [ ] VkFrameBoundaryEXT
 * [ ] VkFrameBoundaryFlagBitsEXT
 * [ ] VkFrameBoundaryFlagsEXT
 * [x] VkFramebuffer
 * [ ] VkFramebufferAttachmentImageInfo
 * [ ] VkFramebufferAttachmentImageInfoKHR
 * [ ] VkFramebufferAttachmentsCreateInfo
 * [ ] VkFramebufferAttachmentsCreateInfoKHR
 * [ ] VkFramebufferCreateFlagBits
 * [ ] VkFramebufferCreateFlags
 * [x] VkFramebufferCreateInfo
 * [ ] VkFramebufferMixedSamplesCombinationNV
 * [ ] VkFrontFace
 * [ ] VkGeneratedCommandsInfoNV
 * [ ] VkGeneratedCommandsMemoryRequirementsInfoNV
 * [ ] VkGeometryAABBNV
 * [ ] VkGeometryDataNV
 * [ ] VkGeometryFlagBitsKHR
 * [ ] VkGeometryFlagBitsNV
 * [ ] VkGeometryFlagsKHR
 * [ ] VkGeometryFlagsNV
 * [ ] VkGeometryInstanceFlagBitsKHR
 * [ ] VkGeometryInstanceFlagBitsNV
 * [ ] VkGeometryInstanceFlagsKHR
 * [ ] VkGeometryInstanceFlagsNV
 * [ ] VkGeometryNV
 * [ ] VkGeometryTrianglesNV
 * [ ] VkGeometryTypeKHR
 * [ ] VkGeometryTypeNV
 * [ ] VkGetLatencyMarkerInfoNV
 * [x] VkGraphicsPipelineCreateInfo
 * [ ] VkGraphicsPipelineLibraryCreateInfoEXT
 * [ ] VkGraphicsPipelineLibraryFlagBitsEXT
 * [ ] VkGraphicsPipelineLibraryFlagsEXT
 * [ ] VkGraphicsPipelineShaderGroupsCreateInfoNV
 * [ ] VkGraphicsShaderGroupCreateInfoNV
 * [ ] VkHdrMetadataEXT
 * [ ] VkHeadlessSurfaceCreateFlagsEXT
 * [ ] VkHeadlessSurfaceCreateInfoEXT
 * [ ] VkHostImageCopyDevicePerformanceQueryEXT
 * [ ] VkHostImageCopyFlagBitsEXT
 * [ ] VkHostImageCopyFlagsEXT
 * [ ] VkHostImageLayoutTransitionInfoEXT
 * [x] VkImage
 * [ ] VkImageAspectFlagBits
 * [ ] VkImageAspectFlags
 * [ ] VkImageBlit
 * [ ] VkImageBlit2
 * [ ] VkImageBlit2KHR
 * [ ] VkImageCaptureDescriptorDataInfoEXT
 * [ ] VkImageCompressionControlEXT
 * [ ] VkImageCompressionFixedRateFlagBitsEXT
 * [ ] VkImageCompressionFixedRateFlagsEXT
 * [ ] VkImageCompressionFlagBitsEXT
 * [ ] VkImageCompressionFlagsEXT
 * [ ] VkImageCompressionPropertiesEXT
 * [ ] VkImageCopy
 * [ ] VkImageCopy2
 * [ ] VkImageCopy2KHR
 * [ ] VkImageCreateFlagBits
 * [ ] VkImageCreateFlags
 * [ ] VkImageCreateInfo
 * [ ] VkImageDrmFormatModifierExplicitCreateInfoEXT
 * [ ] VkImageDrmFormatModifierListCreateInfoEXT
 * [ ] VkImageDrmFormatModifierPropertiesEXT
 * [ ] VkImageFormatListCreateInfo
 * [ ] VkImageFormatListCreateInfoKHR
 * [ ] VkImageFormatProperties
 * [ ] VkImageFormatProperties2
 * [ ] VkImageFormatProperties2KHR
 * [ ] VkImageLayout
 * [ ] VkImageMemoryBarrier
 * [ ] VkImageMemoryBarrier2
 * [ ] VkImageMemoryBarrier2KHR
 * [ ] VkImageMemoryRequirementsInfo2
 * [ ] VkImageMemoryRequirementsInfo2KHR
 * [ ] VkImagePlaneMemoryRequirementsInfo
 * [ ] VkImagePlaneMemoryRequirementsInfoKHR
 * [ ] VkImageResolve
 * [ ] VkImageResolve2
 * [ ] VkImageResolve2KHR
 * [ ] VkImageSparseMemoryRequirementsInfo2
 * [ ] VkImageSparseMemoryRequirementsInfo2KHR
 * [ ] VkImageStencilUsageCreateInfo
 * [ ] VkImageStencilUsageCreateInfoEXT
 * [ ] VkImageSubresource
 * [ ] VkImageSubresource2EXT
 * [ ] VkImageSubresource2KHR
 * [ ] VkImageSubresourceLayers
 * [ ] VkImageSubresourceRange
 * [ ] VkImageSwapchainCreateInfoKHR
 * [ ] VkImageTiling
 * [ ] VkImageToMemoryCopyEXT
 * [ ] VkImageType
 * [ ] VkImageUsageFlagBits
 * [ ] VkImageUsageFlags
 * [x] VkImageView
 * [ ] VkImageViewAddressPropertiesNVX
 * [ ] VkImageViewASTCDecodeModeEXT
 * [ ] VkImageViewCaptureDescriptorDataInfoEXT
 * [ ] VkImageViewCreateFlagBits
 * [ ] VkImageViewCreateFlags
 * [x] VkImageViewCreateInfo
 * [ ] VkImageViewHandleInfoNVX
 * [ ] VkImageViewMinLodCreateInfoEXT
 * [ ] VkImageViewSampleWeightCreateInfoQCOM
 * [ ] VkImageViewSlicedCreateInfoEXT
 * [ ] VkImageViewType
 * [ ] VkImageViewUsageCreateInfo
 * [ ] VkImageViewUsageCreateInfoKHR
 * [ ] VkImportFenceFdInfoKHR
 * [ ] VkImportMemoryFdInfoKHR
 * [ ] VkImportMemoryHostPointerInfoEXT
 * [ ] VkImportSemaphoreFdInfoKHR
 * [ ] VkIndexType
 * [ ] VkIndirectCommandsLayoutCreateInfoNV
 * [ ] VkIndirectCommandsLayoutNV
 * [ ] VkIndirectCommandsLayoutTokenNV
 * [ ] VkIndirectCommandsLayoutUsageFlagBitsNV
 * [ ] VkIndirectCommandsLayoutUsageFlagsNV
 * [ ] VkIndirectCommandsStreamNV
 * [ ] VkIndirectCommandsTokenTypeNV
 * [ ] VkIndirectStateFlagBitsNV
 * [ ] VkIndirectStateFlagsNV
 * [ ] VkInitializePerformanceApiInfoINTEL
 * [ ] VkInputAttachmentAspectReference
 * [ ] VkInputAttachmentAspectReferenceKHR
 * [x] VkInstance
 * [ ] VkInstanceCreateFlagBits
 * [ ] VkInstanceCreateFlags
 * [x] VkInstanceCreateInfo
 * [ ] VkInternalAllocationType
 * [ ] VkLatencyMarkerNV
 * [ ] VkLatencySleepInfoNV
 * [ ] VkLatencySleepModeInfoNV
 * [ ] VkLatencySubmissionPresentIdNV
 * [ ] VkLatencySurfaceCapabilitiesNV
 * [ ] VkLatencyTimingsFrameReportNV
 * [ ] VkLayeredDriverUnderlyingApiMSFT
 * [x] VkLayerProperties
 * [ ] VkLayerSettingEXT
 * [ ] VkLayerSettingsCreateInfoEXT
 * [ ] VkLayerSettingTypeEXT
 * [ ] VkLineRasterizationModeEXT
 * [ ] VkLogicOp
 * [ ] VkMappedMemoryRange
 * [ ] VkMemoryAllocateFlagBits
 * [ ] VkMemoryAllocateFlagBitsKHR
 * [ ] VkMemoryAllocateFlags
 * [ ] VkMemoryAllocateFlagsInfo
 * [ ] VkMemoryAllocateFlagsInfoKHR
 * [ ] VkMemoryAllocateFlagsKHR
 * [x] VkMemoryAllocateInfo
 * [ ] VkMemoryBarrier
 * [ ] VkMemoryBarrier2
 * [ ] VkMemoryBarrier2KHR
 * [ ] VkMemoryDecompressionMethodFlagBitsNV
 * [ ] VkMemoryDecompressionMethodFlagsNV
 * [ ] VkMemoryDedicatedAllocateInfo
 * [ ] VkMemoryDedicatedAllocateInfoKHR
 * [ ] VkMemoryDedicatedRequirements
 * [ ] VkMemoryDedicatedRequirementsKHR
 * [ ] VkMemoryFdPropertiesKHR
 * [ ] VkMemoryGetFdInfoKHR
 * [ ] VkMemoryGetRemoteAddressInfoNV
 * [ ] VkMemoryHeap
 * [ ] VkMemoryHeapFlagBits
 * [ ] VkMemoryHeapFlags
 * [ ] VkMemoryHostPointerPropertiesEXT
 * [ ] VkMemoryMapFlags
 * [ ] VkMemoryMapInfoKHR
 * [ ] VkMemoryOpaqueCaptureAddressAllocateInfo
 * [ ] VkMemoryOpaqueCaptureAddressAllocateInfoKHR
 * [ ] VkMemoryOverallocationBehaviorAMD
 * [ ] VkMemoryPriorityAllocateInfoEXT
 * [ ] VkMemoryPropertyFlagBits
 * [x] VkMemoryPropertyFlags
 * [x] VkMemoryRequirements
 * [ ] VkMemoryRequirements2
 * [ ] VkMemoryRequirements2KHR
 * [ ] VkMemoryToImageCopyEXT
 * [ ] VkMemoryType
 * [ ] VkMemoryUnmapFlagsKHR
 * [ ] VkMemoryUnmapInfoKHR
 * [ ] VkMicromapBuildInfoEXT
 * [ ] VkMicromapBuildSizesInfoEXT
 * [ ] VkMicromapCreateFlagBitsEXT
 * [ ] VkMicromapCreateFlagsEXT
 * [ ] VkMicromapCreateInfoEXT
 * [ ] VkMicromapEXT
 * [ ] VkMicromapTriangleEXT
 * [ ] VkMicromapTypeEXT
 * [ ] VkMicromapUsageEXT
 * [ ] VkMicromapVersionInfoEXT
 * [ ] VkMultiDrawIndexedInfoEXT
 * [ ] VkMultiDrawInfoEXT
 * [ ] VkMultisampledRenderToSingleSampledInfoEXT
 * [ ] VkMultisamplePropertiesEXT
 * [ ] VkMultiviewPerViewAttributesInfoNVX
 * [ ] VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM
 * [ ] VkMutableDescriptorTypeCreateInfoEXT
 * [ ] VkMutableDescriptorTypeCreateInfoVALVE
 * [ ] VkMutableDescriptorTypeListEXT
 * [ ] VkMutableDescriptorTypeListVALVE
 * [ ] VkObjectType
 * [ ] VkOffset2D
 * [ ] VkOffset3D
 * [ ] VkOpacityMicromapFormatEXT
 * [ ] VkOpacityMicromapSpecialIndexEXT
 * [ ] VkOpaqueCaptureDescriptorDataCreateInfoEXT
 * [ ] VkOpticalFlowExecuteFlagBitsNV
 * [ ] VkOpticalFlowExecuteFlagsNV
 * [ ] VkOpticalFlowExecuteInfoNV
 * [ ] VkOpticalFlowGridSizeFlagBitsNV
 * [ ] VkOpticalFlowGridSizeFlagsNV
 * [ ] VkOpticalFlowImageFormatInfoNV
 * [ ] VkOpticalFlowImageFormatPropertiesNV
 * [ ] VkOpticalFlowPerformanceLevelNV
 * [ ] VkOpticalFlowSessionBindingPointNV
 * [ ] VkOpticalFlowSessionCreateFlagBitsNV
 * [ ] VkOpticalFlowSessionCreateFlagsNV
 * [ ] VkOpticalFlowSessionCreateInfoNV
 * [ ] VkOpticalFlowSessionCreatePrivateDataInfoNV
 * [ ] VkOpticalFlowSessionNV
 * [ ] VkOpticalFlowUsageFlagBitsNV
 * [ ] VkOpticalFlowUsageFlagsNV
 * [ ] VkOutOfBandQueueTypeInfoNV
 * [ ] VkOutOfBandQueueTypeNV
 * [ ] VkPastPresentationTimingGOOGLE
 * [ ] VkPeerMemoryFeatureFlagBits
 * [ ] VkPeerMemoryFeatureFlagBitsKHR
 * [ ] VkPeerMemoryFeatureFlags
 * [ ] VkPeerMemoryFeatureFlagsKHR
 * [ ] VkPerformanceConfigurationAcquireInfoINTEL
 * [ ] VkPerformanceConfigurationINTEL
 * [ ] VkPerformanceConfigurationTypeINTEL
 * [ ] VkPerformanceCounterDescriptionFlagBitsKHR
 * [ ] VkPerformanceCounterDescriptionFlagsKHR
 * [ ] VkPerformanceCounterDescriptionKHR
 * [ ] VkPerformanceCounterKHR
 * [ ] VkPerformanceCounterResultKHR
 * [ ] VkPerformanceCounterScopeKHR
 * [ ] VkPerformanceCounterStorageKHR
 * [ ] VkPerformanceCounterUnitKHR
 * [ ] VkPerformanceMarkerInfoINTEL
 * [ ] VkPerformanceOverrideInfoINTEL
 * [ ] VkPerformanceOverrideTypeINTEL
 * [ ] VkPerformanceParameterTypeINTEL
 * [ ] VkPerformanceQuerySubmitInfoKHR
 * [ ] VkPerformanceStreamMarkerInfoINTEL
 * [ ] VkPerformanceValueDataINTEL
 * [ ] VkPerformanceValueINTEL
 * [ ] VkPerformanceValueTypeINTEL
 * [x] VkPhysicalDevice
 * [ ] VkPhysicalDevice16BitStorageFeatures
 * [ ] VkPhysicalDevice16BitStorageFeaturesKHR
 * [ ] VkPhysicalDevice4444FormatsFeaturesEXT
 * [ ] VkPhysicalDevice8BitStorageFeatures
 * [ ] VkPhysicalDevice8BitStorageFeaturesKHR
 * [ ] VkPhysicalDeviceAccelerationStructureFeaturesKHR
 * [ ] VkPhysicalDeviceAccelerationStructurePropertiesKHR
 * [ ] VkPhysicalDeviceAddressBindingReportFeaturesEXT
 * [ ] VkPhysicalDeviceAmigoProfilingFeaturesSEC
 * [ ] VkPhysicalDeviceASTCDecodeFeaturesEXT
 * [ ] VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT
 * [ ] VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT
 * [ ] VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT
 * [ ] VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT
 * [ ] VkPhysicalDeviceBorderColorSwizzleFeaturesEXT
 * [ ] VkPhysicalDeviceBufferAddressFeaturesEXT
 * [ ] VkPhysicalDeviceBufferDeviceAddressFeatures
 * [ ] VkPhysicalDeviceBufferDeviceAddressFeaturesEXT
 * [ ] VkPhysicalDeviceBufferDeviceAddressFeaturesKHR
 * [ ] VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI
 * [ ] VkPhysicalDeviceClusterCullingShaderPropertiesHUAWEI
 * [ ] VkPhysicalDeviceClusterCullingShaderVrsFeaturesHUAWEI
 * [ ] VkPhysicalDeviceCoherentMemoryFeaturesAMD
 * [ ] VkPhysicalDeviceColorWriteEnableFeaturesEXT
 * [ ] VkPhysicalDeviceComputeShaderDerivativesFeaturesNV
 * [ ] VkPhysicalDeviceConditionalRenderingFeaturesEXT
 * [ ] VkPhysicalDeviceConservativeRasterizationPropertiesEXT
 * [ ] VkPhysicalDeviceCooperativeMatrixFeaturesKHR
 * [ ] VkPhysicalDeviceCooperativeMatrixFeaturesNV
 * [ ] VkPhysicalDeviceCooperativeMatrixPropertiesKHR
 * [ ] VkPhysicalDeviceCooperativeMatrixPropertiesNV
 * [ ] VkPhysicalDeviceCopyMemoryIndirectFeaturesNV
 * [ ] VkPhysicalDeviceCopyMemoryIndirectPropertiesNV
 * [ ] VkPhysicalDeviceCornerSampledImageFeaturesNV
 * [ ] VkPhysicalDeviceCoverageReductionModeFeaturesNV
 * [ ] VkPhysicalDeviceCubicClampFeaturesQCOM
 * [ ] VkPhysicalDeviceCubicWeightsFeaturesQCOM
 * [ ] VkPhysicalDeviceCudaKernelLaunchFeaturesNV
 * [ ] VkPhysicalDeviceCudaKernelLaunchPropertiesNV
 * [ ] VkPhysicalDeviceCustomBorderColorFeaturesEXT
 * [ ] VkPhysicalDeviceCustomBorderColorPropertiesEXT
 * [ ] VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV
 * [ ] VkPhysicalDeviceDepthBiasControlFeaturesEXT
 * [ ] VkPhysicalDeviceDepthClampZeroOneFeaturesEXT
 * [ ] VkPhysicalDeviceDepthClipControlFeaturesEXT
 * [ ] VkPhysicalDeviceDepthClipEnableFeaturesEXT
 * [ ] VkPhysicalDeviceDepthStencilResolveProperties
 * [ ] VkPhysicalDeviceDepthStencilResolvePropertiesKHR
 * [ ] VkPhysicalDeviceDescriptorBufferDensityMapPropertiesEXT
 * [ ] VkPhysicalDeviceDescriptorBufferFeaturesEXT
 * [ ] VkPhysicalDeviceDescriptorBufferPropertiesEXT
 * [ ] VkPhysicalDeviceDescriptorIndexingFeatures
 * [ ] VkPhysicalDeviceDescriptorIndexingFeaturesEXT
 * [ ] VkPhysicalDeviceDescriptorIndexingProperties
 * [ ] VkPhysicalDeviceDescriptorIndexingPropertiesEXT
 * [ ] VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV
 * [ ] VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE
 * [ ] VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV
 * [ ] VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV
 * [ ] VkPhysicalDeviceDeviceGeneratedCommandsPropertiesNV
 * [ ] VkPhysicalDeviceDeviceMemoryReportFeaturesEXT
 * [ ] VkPhysicalDeviceDiagnosticsConfigFeaturesNV
 * [ ] VkPhysicalDeviceDiscardRectanglePropertiesEXT
 * [ ] VkPhysicalDeviceDriverProperties
 * [ ] VkPhysicalDeviceDriverPropertiesKHR
 * [ ] VkPhysicalDeviceDrmPropertiesEXT
 * [ ] VkPhysicalDeviceDynamicRenderingFeatures
 * [ ] VkPhysicalDeviceDynamicRenderingFeaturesKHR
 * [ ] VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT
 * [ ] VkPhysicalDeviceExclusiveScissorFeaturesNV
 * [ ] VkPhysicalDeviceExtendedDynamicState2FeaturesEXT
 * [ ] VkPhysicalDeviceExtendedDynamicState3FeaturesEXT
 * [ ] VkPhysicalDeviceExtendedDynamicState3PropertiesEXT
 * [ ] VkPhysicalDeviceExtendedDynamicStateFeaturesEXT
 * [ ] VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV
 * [ ] VkPhysicalDeviceExtendedSparseAddressSpacePropertiesNV
 * [ ] VkPhysicalDeviceExternalBufferInfo
 * [ ] VkPhysicalDeviceExternalBufferInfoKHR
 * [ ] VkPhysicalDeviceExternalFenceInfo
 * [ ] VkPhysicalDeviceExternalFenceInfoKHR
 * [ ] VkPhysicalDeviceExternalImageFormatInfo
 * [ ] VkPhysicalDeviceExternalImageFormatInfoKHR
 * [ ] VkPhysicalDeviceExternalMemoryHostPropertiesEXT
 * [ ] VkPhysicalDeviceExternalMemoryRDMAFeaturesNV
 * [ ] VkPhysicalDeviceExternalSemaphoreInfo
 * [ ] VkPhysicalDeviceExternalSemaphoreInfoKHR
 * [ ] VkPhysicalDeviceFaultFeaturesEXT
 * [x] VkPhysicalDeviceFeatures
 * [ ] VkPhysicalDeviceFeatures2
 * [ ] VkPhysicalDeviceFeatures2KHR
 * [ ] VkPhysicalDeviceFloat16Int8FeaturesKHR
 * [ ] VkPhysicalDeviceFloatControlsProperties
 * [ ] VkPhysicalDeviceFloatControlsPropertiesKHR
 * [ ] VkPhysicalDeviceFragmentDensityMap2FeaturesEXT
 * [ ] VkPhysicalDeviceFragmentDensityMap2PropertiesEXT
 * [ ] VkPhysicalDeviceFragmentDensityMapFeaturesEXT
 * [ ] VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM
 * [ ] VkPhysicalDeviceFragmentDensityMapOffsetPropertiesQCOM
 * [ ] VkPhysicalDeviceFragmentDensityMapPropertiesEXT
 * [ ] VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR
 * [ ] VkPhysicalDeviceFragmentShaderBarycentricFeaturesNV
 * [ ] VkPhysicalDeviceFragmentShaderBarycentricPropertiesKHR
 * [ ] VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT
 * [ ] VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV
 * [ ] VkPhysicalDeviceFragmentShadingRateEnumsPropertiesNV
 * [ ] VkPhysicalDeviceFragmentShadingRateFeaturesKHR
 * [ ] VkPhysicalDeviceFragmentShadingRateKHR
 * [ ] VkPhysicalDeviceFragmentShadingRatePropertiesKHR
 * [ ] VkPhysicalDeviceFrameBoundaryFeaturesEXT
 * [ ] VkPhysicalDeviceGlobalPriorityQueryFeaturesEXT
 * [ ] VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR
 * [ ] VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT
 * [ ] VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT
 * [ ] VkPhysicalDeviceGroupProperties
 * [ ] VkPhysicalDeviceGroupPropertiesKHR
 * [ ] VkPhysicalDeviceHostImageCopyFeaturesEXT
 * [ ] VkPhysicalDeviceHostImageCopyPropertiesEXT
 * [ ] VkPhysicalDeviceHostQueryResetFeatures
 * [ ] VkPhysicalDeviceHostQueryResetFeaturesEXT
 * [ ] VkPhysicalDeviceIDProperties
 * [ ] VkPhysicalDeviceIDPropertiesKHR
 * [ ] VkPhysicalDeviceImage2DViewOf3DFeaturesEXT
 * [ ] VkPhysicalDeviceImageCompressionControlFeaturesEXT
 * [ ] VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT
 * [ ] VkPhysicalDeviceImageDrmFormatModifierInfoEXT
 * [ ] VkPhysicalDeviceImageFormatInfo2
 * [ ] VkPhysicalDeviceImageFormatInfo2KHR
 * [ ] VkPhysicalDeviceImagelessFramebufferFeatures
 * [ ] VkPhysicalDeviceImagelessFramebufferFeaturesKHR
 * [ ] VkPhysicalDeviceImageProcessing2FeaturesQCOM
 * [ ] VkPhysicalDeviceImageProcessing2PropertiesQCOM
 * [ ] VkPhysicalDeviceImageProcessingFeaturesQCOM
 * [ ] VkPhysicalDeviceImageProcessingPropertiesQCOM
 * [ ] VkPhysicalDeviceImageRobustnessFeatures
 * [ ] VkPhysicalDeviceImageRobustnessFeaturesEXT
 * [ ] VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT
 * [ ] VkPhysicalDeviceImageViewImageFormatInfoEXT
 * [ ] VkPhysicalDeviceImageViewMinLodFeaturesEXT
 * [ ] VkPhysicalDeviceIndexTypeUint8FeaturesEXT
 * [ ] VkPhysicalDeviceInheritedViewportScissorFeaturesNV
 * [ ] VkPhysicalDeviceInlineUniformBlockFeatures
 * [ ] VkPhysicalDeviceInlineUniformBlockFeaturesEXT
 * [ ] VkPhysicalDeviceInlineUniformBlockProperties
 * [ ] VkPhysicalDeviceInlineUniformBlockPropertiesEXT
 * [ ] VkPhysicalDeviceInvocationMaskFeaturesHUAWEI
 * [ ] VkPhysicalDeviceLayeredDriverPropertiesMSFT
 * [ ] VkPhysicalDeviceLegacyDitheringFeaturesEXT
 * [ ] VkPhysicalDeviceLimits
 * [ ] VkPhysicalDeviceLinearColorAttachmentFeaturesNV
 * [ ] VkPhysicalDeviceLineRasterizationFeaturesEXT
 * [ ] VkPhysicalDeviceLineRasterizationPropertiesEXT
 * [ ] VkPhysicalDeviceMaintenance3Properties
 * [ ] VkPhysicalDeviceMaintenance3PropertiesKHR
 * [ ] VkPhysicalDeviceMaintenance4Features
 * [ ] VkPhysicalDeviceMaintenance4FeaturesKHR
 * [ ] VkPhysicalDeviceMaintenance4Properties
 * [ ] VkPhysicalDeviceMaintenance4PropertiesKHR
 * [ ] VkPhysicalDeviceMaintenance5FeaturesKHR
 * [ ] VkPhysicalDeviceMaintenance5PropertiesKHR
 * [ ] VkPhysicalDeviceMaintenance6FeaturesKHR
 * [ ] VkPhysicalDeviceMaintenance6PropertiesKHR
 * [ ] VkPhysicalDeviceMemoryBudgetPropertiesEXT
 * [ ] VkPhysicalDeviceMemoryDecompressionFeaturesNV
 * [ ] VkPhysicalDeviceMemoryDecompressionPropertiesNV
 * [ ] VkPhysicalDeviceMemoryPriorityFeaturesEXT
 * [x] VkPhysicalDeviceMemoryProperties
 * [ ] VkPhysicalDeviceMemoryProperties2
 * [ ] VkPhysicalDeviceMemoryProperties2KHR
 * [ ] VkPhysicalDeviceMeshShaderFeaturesEXT
 * [ ] VkPhysicalDeviceMeshShaderFeaturesNV
 * [ ] VkPhysicalDeviceMeshShaderPropertiesEXT
 * [ ] VkPhysicalDeviceMeshShaderPropertiesNV
 * [ ] VkPhysicalDeviceMultiDrawFeaturesEXT
 * [ ] VkPhysicalDeviceMultiDrawPropertiesEXT
 * [ ] VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT
 * [ ] VkPhysicalDeviceMultiviewFeatures
 * [ ] VkPhysicalDeviceMultiviewFeaturesKHR
 * [ ] VkPhysicalDeviceMultiviewPerViewAttributesPropertiesNVX
 * [ ] VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM
 * [ ] VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM
 * [ ] VkPhysicalDeviceMultiviewProperties
 * [ ] VkPhysicalDeviceMultiviewPropertiesKHR
 * [ ] VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT
 * [ ] VkPhysicalDeviceMutableDescriptorTypeFeaturesVALVE
 * [ ] VkPhysicalDeviceNestedCommandBufferFeaturesEXT
 * [ ] VkPhysicalDeviceNestedCommandBufferPropertiesEXT
 * [ ] VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT
 * [ ] VkPhysicalDeviceOpacityMicromapFeaturesEXT
 * [ ] VkPhysicalDeviceOpacityMicromapPropertiesEXT
 * [ ] VkPhysicalDeviceOpticalFlowFeaturesNV
 * [ ] VkPhysicalDeviceOpticalFlowPropertiesNV
 * [ ] VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT
 * [ ] VkPhysicalDevicePCIBusInfoPropertiesEXT
 * [ ] VkPhysicalDevicePerformanceQueryFeaturesKHR
 * [ ] VkPhysicalDevicePerformanceQueryPropertiesKHR
 * [ ] VkPhysicalDevicePerStageDescriptorSetFeaturesNV
 * [ ] VkPhysicalDevicePipelineCreationCacheControlFeatures
 * [ ] VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT
 * [ ] VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR
 * [ ] VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT
 * [ ] VkPhysicalDevicePipelinePropertiesFeaturesEXT
 * [ ] VkPhysicalDevicePipelineProtectedAccessFeaturesEXT
 * [ ] VkPhysicalDevicePipelineRobustnessFeaturesEXT
 * [ ] VkPhysicalDevicePipelineRobustnessPropertiesEXT
 * [ ] VkPhysicalDevicePointClippingProperties
 * [ ] VkPhysicalDevicePointClippingPropertiesKHR
 * [ ] VkPhysicalDevicePresentBarrierFeaturesNV
 * [ ] VkPhysicalDevicePresentIdFeaturesKHR
 * [ ] VkPhysicalDevicePresentWaitFeaturesKHR
 * [ ] VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT
 * [ ] VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT
 * [ ] VkPhysicalDevicePrivateDataFeatures
 * [ ] VkPhysicalDevicePrivateDataFeaturesEXT
 * [x] VkPhysicalDeviceProperties
 * [ ] VkPhysicalDeviceProperties2
 * [ ] VkPhysicalDeviceProperties2KHR
 * [ ] VkPhysicalDeviceProtectedMemoryFeatures
 * [ ] VkPhysicalDeviceProtectedMemoryProperties
 * [ ] VkPhysicalDeviceProvokingVertexFeaturesEXT
 * [ ] VkPhysicalDeviceProvokingVertexPropertiesEXT
 * [ ] VkPhysicalDevicePushDescriptorPropertiesKHR
 * [ ] VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesARM
 * [ ] VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT
 * [ ] VkPhysicalDeviceRayQueryFeaturesKHR
 * [ ] VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV
 * [ ] VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV
 * [ ] VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR
 * [ ] VkPhysicalDeviceRayTracingMotionBlurFeaturesNV
 * [ ] VkPhysicalDeviceRayTracingPipelineFeaturesKHR
 * [ ] VkPhysicalDeviceRayTracingPipelinePropertiesKHR
 * [ ] VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR
 * [ ] VkPhysicalDeviceRayTracingPropertiesNV
 * [ ] VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG
 * [ ] VkPhysicalDeviceRenderPassStripedFeaturesARM
 * [ ] VkPhysicalDeviceRenderPassStripedPropertiesARM
 * [ ] VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV
 * [ ] VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT
 * [ ] VkPhysicalDeviceRobustness2FeaturesEXT
 * [ ] VkPhysicalDeviceRobustness2PropertiesEXT
 * [ ] VkPhysicalDeviceSampleLocationsPropertiesEXT
 * [ ] VkPhysicalDeviceSamplerFilterMinmaxProperties
 * [ ] VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT
 * [ ] VkPhysicalDeviceSamplerYcbcrConversionFeatures
 * [ ] VkPhysicalDeviceSamplerYcbcrConversionFeaturesKHR
 * [ ] VkPhysicalDeviceScalarBlockLayoutFeatures
 * [ ] VkPhysicalDeviceScalarBlockLayoutFeaturesEXT
 * [ ] VkPhysicalDeviceSchedulingControlsFeaturesARM
 * [ ] VkPhysicalDeviceSchedulingControlsFlagBitsARM
 * [ ] VkPhysicalDeviceSchedulingControlsFlagsARM
 * [ ] VkPhysicalDeviceSchedulingControlsPropertiesARM
 * [ ] VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures
 * [ ] VkPhysicalDeviceSeparateDepthStencilLayoutsFeaturesKHR
 * [ ] VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT
 * [ ] VkPhysicalDeviceShaderAtomicFloatFeaturesEXT
 * [ ] VkPhysicalDeviceShaderAtomicInt64Features
 * [ ] VkPhysicalDeviceShaderAtomicInt64FeaturesKHR
 * [ ] VkPhysicalDeviceShaderClockFeaturesKHR
 * [ ] VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM
 * [ ] VkPhysicalDeviceShaderCoreBuiltinsPropertiesARM
 * [ ] VkPhysicalDeviceShaderCoreProperties2AMD
 * [ ] VkPhysicalDeviceShaderCorePropertiesAMD
 * [ ] VkPhysicalDeviceShaderCorePropertiesARM
 * [ ] VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures
 * [ ] VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT
 * [ ] VkPhysicalDeviceShaderDrawParameterFeatures
 * [ ] VkPhysicalDeviceShaderDrawParametersFeatures
 * [ ] VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD
 * [ ] VkPhysicalDeviceShaderFloat16Int8Features
 * [ ] VkPhysicalDeviceShaderFloat16Int8FeaturesKHR
 * [ ] VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT
 * [ ] VkPhysicalDeviceShaderImageFootprintFeaturesNV
 * [ ] VkPhysicalDeviceShaderIntegerDotProductFeatures
 * [ ] VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR
 * [ ] VkPhysicalDeviceShaderIntegerDotProductProperties
 * [ ] VkPhysicalDeviceShaderIntegerDotProductPropertiesKHR
 * [ ] VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL
 * [ ] VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT
 * [ ] VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT
 * [ ] VkPhysicalDeviceShaderObjectFeaturesEXT
 * [ ] VkPhysicalDeviceShaderObjectPropertiesEXT
 * [ ] VkPhysicalDeviceShaderSMBuiltinsFeaturesNV
 * [ ] VkPhysicalDeviceShaderSMBuiltinsPropertiesNV
 * [ ] VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures
 * [ ] VkPhysicalDeviceShaderSubgroupExtendedTypesFeaturesKHR
 * [ ] VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR
 * [ ] VkPhysicalDeviceShaderTerminateInvocationFeatures
 * [ ] VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR
 * [ ] VkPhysicalDeviceShaderTileImageFeaturesEXT
 * [ ] VkPhysicalDeviceShaderTileImagePropertiesEXT
 * [ ] VkPhysicalDeviceShadingRateImageFeaturesNV
 * [ ] VkPhysicalDeviceShadingRateImagePropertiesNV
 * [ ] VkPhysicalDeviceSparseImageFormatInfo2
 * [ ] VkPhysicalDeviceSparseImageFormatInfo2KHR
 * [ ] VkPhysicalDeviceSparseProperties
 * [ ] VkPhysicalDeviceSubgroupProperties
 * [ ] VkPhysicalDeviceSubgroupSizeControlFeatures
 * [ ] VkPhysicalDeviceSubgroupSizeControlFeaturesEXT
 * [ ] VkPhysicalDeviceSubgroupSizeControlProperties
 * [ ] VkPhysicalDeviceSubgroupSizeControlPropertiesEXT
 * [ ] VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT
 * [ ] VkPhysicalDeviceSubpassShadingFeaturesHUAWEI
 * [ ] VkPhysicalDeviceSubpassShadingPropertiesHUAWEI
 * [ ] VkPhysicalDeviceSurfaceInfo2KHR
 * [ ] VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT
 * [ ] VkPhysicalDeviceSynchronization2Features
 * [ ] VkPhysicalDeviceSynchronization2FeaturesKHR
 * [ ] VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT
 * [ ] VkPhysicalDeviceTexelBufferAlignmentProperties
 * [ ] VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT
 * [ ] VkPhysicalDeviceTextureCompressionASTCHDRFeatures
 * [ ] VkPhysicalDeviceTextureCompressionASTCHDRFeaturesEXT
 * [ ] VkPhysicalDeviceTilePropertiesFeaturesQCOM
 * [ ] VkPhysicalDeviceTimelineSemaphoreFeatures
 * [ ] VkPhysicalDeviceTimelineSemaphoreFeaturesKHR
 * [ ] VkPhysicalDeviceTimelineSemaphoreProperties
 * [ ] VkPhysicalDeviceTimelineSemaphorePropertiesKHR
 * [ ] VkPhysicalDeviceToolProperties
 * [ ] VkPhysicalDeviceToolPropertiesEXT
 * [ ] VkPhysicalDeviceTransformFeedbackFeaturesEXT
 * [ ] VkPhysicalDeviceTransformFeedbackPropertiesEXT
 * [x] VkPhysicalDeviceType
 * [ ] VkPhysicalDeviceUniformBufferStandardLayoutFeatures
 * [ ] VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR
 * [ ] VkPhysicalDeviceVariablePointerFeatures
 * [ ] VkPhysicalDeviceVariablePointerFeaturesKHR
 * [ ] VkPhysicalDeviceVariablePointersFeatures
 * [ ] VkPhysicalDeviceVariablePointersFeaturesKHR
 * [ ] VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT
 * [ ] VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR
 * [ ] VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT
 * [ ] VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR
 * [ ] VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT
 * [ ] VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR
 * [ ] VkPhysicalDeviceVideoFormatInfoKHR
 * [ ] VkPhysicalDeviceVideoMaintenance1FeaturesKHR
 * [ ] VkPhysicalDeviceVulkan11Features
 * [ ] VkPhysicalDeviceVulkan11Properties
 * [ ] VkPhysicalDeviceVulkan12Features
 * [ ] VkPhysicalDeviceVulkan12Properties
 * [ ] VkPhysicalDeviceVulkan13Features
 * [ ] VkPhysicalDeviceVulkan13Properties
 * [ ] VkPhysicalDeviceVulkanMemoryModelFeatures
 * [ ] VkPhysicalDeviceVulkanMemoryModelFeaturesKHR
 * [ ] VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR
 * [ ] VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT
 * [ ] VkPhysicalDeviceYcbcrDegammaFeaturesQCOM
 * [ ] VkPhysicalDeviceYcbcrImageArraysFeaturesEXT
 * [ ] VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures
 * [ ] VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR
 * [x] VkPipeline
 * [ ] VkPipelineBindPoint
 * [ ] VkPipelineCache
 * [ ] VkPipelineCacheCreateFlagBits
 * [ ] VkPipelineCacheCreateFlags
 * [ ] VkPipelineCacheCreateInfo
 * [ ] VkPipelineCacheHeaderVersion
 * [ ] VkPipelineCacheHeaderVersionOne
 * [ ] VkPipelineColorBlendAdvancedStateCreateInfoEXT
 * [x] VkPipelineColorBlendAttachmentState
 * [ ] VkPipelineColorBlendStateCreateFlagBits
 * [ ] VkPipelineColorBlendStateCreateFlags
 * [x] VkPipelineColorBlendStateCreateInfo
 * [ ] VkPipelineColorWriteCreateInfoEXT
 * [ ] VkPipelineCompilerControlCreateInfoAMD
 * [ ] VkPipelineCompilerControlFlagBitsAMD
 * [ ] VkPipelineCompilerControlFlagsAMD
 * [ ] VkPipelineCoverageModulationStateCreateFlagsNV
 * [ ] VkPipelineCoverageModulationStateCreateInfoNV
 * [ ] VkPipelineCoverageReductionStateCreateFlagsNV
 * [ ] VkPipelineCoverageReductionStateCreateInfoNV
 * [ ] VkPipelineCoverageToColorStateCreateFlagsNV
 * [ ] VkPipelineCoverageToColorStateCreateInfoNV
 * [ ] VkPipelineCreateFlagBits
 * [ ] VkPipelineCreateFlagBits2KHR
 * [ ] VkPipelineCreateFlags
 * [ ] VkPipelineCreateFlags2CreateInfoKHR
 * [ ] VkPipelineCreateFlags2KHR
 * [ ] VkPipelineCreationFeedback
 * [ ] VkPipelineCreationFeedbackCreateInfo
 * [ ] VkPipelineCreationFeedbackCreateInfoEXT
 * [ ] VkPipelineCreationFeedbackEXT
 * [ ] VkPipelineCreationFeedbackFlagBits
 * [ ] VkPipelineCreationFeedbackFlagBitsEXT
 * [ ] VkPipelineCreationFeedbackFlags
 * [ ] VkPipelineCreationFeedbackFlagsEXT
 * [ ] VkPipelineDepthStencilStateCreateFlagBits
 * [ ] VkPipelineDepthStencilStateCreateFlags
 * [ ] VkPipelineDepthStencilStateCreateInfo
 * [ ] VkPipelineDiscardRectangleStateCreateFlagsEXT
 * [ ] VkPipelineDiscardRectangleStateCreateInfoEXT
 * [ ] VkPipelineDynamicStateCreateFlags
 * [x] VkPipelineDynamicStateCreateInfo
 * [ ] VkPipelineExecutableInfoKHR
 * [ ] VkPipelineExecutableInternalRepresentationKHR
 * [ ] VkPipelineExecutablePropertiesKHR
 * [ ] VkPipelineExecutableStatisticFormatKHR
 * [ ] VkPipelineExecutableStatisticKHR
 * [ ] VkPipelineExecutableStatisticValueKHR
 * [ ] VkPipelineFragmentShadingRateEnumStateCreateInfoNV
 * [ ] VkPipelineFragmentShadingRateStateCreateInfoKHR
 * [ ] VkPipelineIndirectDeviceAddressInfoNV
 * [ ] VkPipelineInfoEXT
 * [ ] VkPipelineInfoKHR
 * [ ] VkPipelineInputAssemblyStateCreateFlags
 * [x] VkPipelineInputAssemblyStateCreateInfo
 * [x] VkPipelineLayout
 * [ ] VkPipelineLayoutCreateFlagBits
 * [ ] VkPipelineLayoutCreateFlags
 * [x] VkPipelineLayoutCreateInfo
 * [ ] VkPipelineLibraryCreateInfoKHR
 * [ ] VkPipelineMultisampleStateCreateFlags
 * [x] VkPipelineMultisampleStateCreateInfo
 * [ ] VkPipelinePropertiesIdentifierEXT
 * [ ] VkPipelineRasterizationConservativeStateCreateFlagsEXT
 * [ ] VkPipelineRasterizationConservativeStateCreateInfoEXT
 * [ ] VkPipelineRasterizationDepthClipStateCreateFlagsEXT
 * [ ] VkPipelineRasterizationDepthClipStateCreateInfoEXT
 * [ ] VkPipelineRasterizationLineStateCreateInfoEXT
 * [ ] VkPipelineRasterizationProvokingVertexStateCreateInfoEXT
 * [ ] VkPipelineRasterizationStateCreateFlags
 * [x] VkPipelineRasterizationStateCreateInfo
 * [ ] VkPipelineRasterizationStateRasterizationOrderAMD
 * [ ] VkPipelineRasterizationStateStreamCreateFlagsEXT
 * [ ] VkPipelineRasterizationStateStreamCreateInfoEXT
 * [ ] VkPipelineRenderingCreateInfo
 * [ ] VkPipelineRenderingCreateInfoKHR
 * [ ] VkPipelineRepresentativeFragmentTestStateCreateInfoNV
 * [ ] VkPipelineRobustnessBufferBehaviorEXT
 * [ ] VkPipelineRobustnessCreateInfoEXT
 * [ ] VkPipelineRobustnessImageBehaviorEXT
 * [ ] VkPipelineSampleLocationsStateCreateInfoEXT
 * [ ] VkPipelineShaderStageCreateFlagBits
 * [ ] VkPipelineShaderStageCreateFlags
 * [x] VkPipelineShaderStageCreateInfo
 * [ ] VkPipelineShaderStageModuleIdentifierCreateInfoEXT
 * [ ] VkPipelineShaderStageRequiredSubgroupSizeCreateInfo
 * [ ] VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT
 * [ ] VkPipelineStageFlagBits
 * [ ] VkPipelineStageFlagBits2
 * [ ] VkPipelineStageFlagBits2KHR
 * [x] VkPipelineStageFlags
 * [ ] VkPipelineStageFlags2
 * [ ] VkPipelineStageFlags2KHR
 * [ ] VkPipelineTessellationDomainOriginStateCreateInfo
 * [ ] VkPipelineTessellationDomainOriginStateCreateInfoKHR
 * [ ] VkPipelineTessellationStateCreateFlags
 * [ ] VkPipelineTessellationStateCreateInfo
 * [ ] VkPipelineVertexInputDivisorStateCreateInfoEXT
 * [ ] VkPipelineVertexInputDivisorStateCreateInfoKHR
 * [ ] VkPipelineVertexInputStateCreateFlags
 * [x] VkPipelineVertexInputStateCreateInfo
 * [ ] VkPipelineViewportCoarseSampleOrderStateCreateInfoNV
 * [ ] VkPipelineViewportDepthClipControlCreateInfoEXT
 * [ ] VkPipelineViewportExclusiveScissorStateCreateInfoNV
 * [ ] VkPipelineViewportShadingRateImageStateCreateInfoNV
 * [ ] VkPipelineViewportStateCreateFlags
 * [x] VkPipelineViewportStateCreateInfo
 * [ ] VkPipelineViewportSwizzleStateCreateFlagsNV
 * [ ] VkPipelineViewportSwizzleStateCreateInfoNV
 * [ ] VkPipelineViewportWScalingStateCreateInfoNV
 * [ ] VkPointClippingBehavior
 * [ ] VkPointClippingBehaviorKHR
 * [ ] VkPolygonMode
 * [ ] VkPresentGravityFlagBitsEXT
 * [ ] VkPresentGravityFlagsEXT
 * [ ] VkPresentIdKHR
 * [x] VkPresentInfoKHR
 * [ ] VkPresentModeKHR
 * [ ] VkPresentRegionKHR
 * [ ] VkPresentRegionsKHR
 * [ ] VkPresentScalingFlagBitsEXT
 * [ ] VkPresentScalingFlagsEXT
 * [ ] VkPresentTimeGOOGLE
 * [ ] VkPresentTimesInfoGOOGLE
 * [x] VkPrimitiveTopology
 * [ ] VkPrivateDataSlot
 * [ ] VkPrivateDataSlotCreateFlags
 * [ ] VkPrivateDataSlotCreateFlagsEXT
 * [ ] VkPrivateDataSlotCreateInfo
 * [ ] VkPrivateDataSlotCreateInfoEXT
 * [ ] VkPrivateDataSlotEXT
 * [ ] VkProtectedSubmitInfo
 * [ ] VkProvokingVertexModeEXT
 * [ ] VkPushConstantRange
 * [ ] VkPushConstantsInfoKHR
 * [ ] VkPushDescriptorSetInfoKHR
 * [ ] VkPushDescriptorSetWithTemplateInfoKHR
 * [ ] VkQueryControlFlagBits
 * [ ] VkQueryControlFlags
 * [ ] VkQueryLowLatencySupportNV
 * [ ] VkQueryPipelineStatisticFlagBits
 * [ ] VkQueryPipelineStatisticFlags
 * [ ] VkQueryPool
 * [ ] VkQueryPoolCreateFlags
 * [ ] VkQueryPoolCreateInfo
 * [ ] VkQueryPoolCreateInfoINTEL
 * [ ] VkQueryPoolPerformanceCreateInfoKHR
 * [ ] VkQueryPoolPerformanceQueryCreateInfoINTEL
 * [ ] VkQueryPoolSamplingModeINTEL
 * [ ] VkQueryPoolVideoEncodeFeedbackCreateInfoKHR
 * [ ] VkQueryResultFlagBits
 * [ ] VkQueryResultFlags
 * [ ] VkQueryResultStatusKHR
 * [ ] VkQueryType
 * [x] VkQueue
 * [ ] VkQueueFamilyCheckpointProperties2NV
 * [ ] VkQueueFamilyCheckpointPropertiesNV
 * [ ] VkQueueFamilyGlobalPriorityPropertiesEXT
 * [ ] VkQueueFamilyGlobalPriorityPropertiesKHR
 * [x] VkQueueFamilyProperties
 * [ ] VkQueueFamilyProperties2
 * [ ] VkQueueFamilyProperties2KHR
 * [ ] VkQueueFamilyQueryResultStatusPropertiesKHR
 * [ ] VkQueueFamilyVideoPropertiesKHR
 * [ ] VkQueueFlagBits
 * [ ] VkQueueFlags
 * [ ] VkQueueGlobalPriorityEXT
 * [ ] VkQueueGlobalPriorityKHR
 * [ ] VkRasterizationOrderAMD
 * [ ] VkRayTracingInvocationReorderModeNV
 * [ ] VkRayTracingPipelineCreateInfoKHR
 * [ ] VkRayTracingPipelineCreateInfoNV
 * [ ] VkRayTracingPipelineInterfaceCreateInfoKHR
 * [ ] VkRayTracingShaderGroupCreateInfoKHR
 * [ ] VkRayTracingShaderGroupCreateInfoNV
 * [ ] VkRayTracingShaderGroupTypeKHR
 * [ ] VkRayTracingShaderGroupTypeNV
 * [x] VkRect2D
 * [ ] VkRectLayerKHR
 * [ ] VkRefreshCycleDurationGOOGLE
 * [ ] VkReleaseSwapchainImagesInfoEXT
 * [ ] VkRenderingAreaInfoKHR
 * [ ] VkRenderingAttachmentInfo
 * [ ] VkRenderingAttachmentInfoKHR
 * [ ] VkRenderingFlagBits
 * [ ] VkRenderingFlagBitsKHR
 * [ ] VkRenderingFlags
 * [ ] VkRenderingFlagsKHR
 * [ ] VkRenderingFragmentDensityMapAttachmentInfoEXT
 * [ ] VkRenderingFragmentShadingRateAttachmentInfoKHR
 * [ ] VkRenderingInfo
 * [ ] VkRenderingInfoKHR
 * [x] VkRenderPass
 * [ ] VkRenderPassAttachmentBeginInfo
 * [ ] VkRenderPassAttachmentBeginInfoKHR
 * [x] VkRenderPassBeginInfo
 * [ ] VkRenderPassCreateFlagBits
 * [ ] VkRenderPassCreateFlags
 * [x] VkRenderPassCreateInfo
 * [ ] VkRenderPassCreateInfo2
 * [ ] VkRenderPassCreateInfo2KHR
 * [ ] VkRenderPassCreationControlEXT
 * [ ] VkRenderPassCreationFeedbackCreateInfoEXT
 * [ ] VkRenderPassCreationFeedbackInfoEXT
 * [ ] VkRenderPassFragmentDensityMapCreateInfoEXT
 * [ ] VkRenderPassInputAttachmentAspectCreateInfo
 * [ ] VkRenderPassInputAttachmentAspectCreateInfoKHR
 * [ ] VkRenderPassMultiviewCreateInfo
 * [ ] VkRenderPassMultiviewCreateInfoKHR
 * [ ] VkRenderPassSampleLocationsBeginInfoEXT
 * [ ] VkRenderPassStripeBeginInfoARM
 * [ ] VkRenderPassStripeInfoARM
 * [ ] VkRenderPassStripeSubmitInfoARM
 * [ ] VkRenderPassSubpassFeedbackCreateInfoEXT
 * [ ] VkRenderPassSubpassFeedbackInfoEXT
 * [ ] VkRenderPassTransformBeginInfoQCOM
 * [ ] VkResolveImageInfo2
 * [ ] VkResolveImageInfo2KHR
 * [ ] VkResolveModeFlagBits
 * [ ] VkResolveModeFlagBitsKHR
 * [ ] VkResolveModeFlags
 * [ ] VkResolveModeFlagsKHR
 * [x] VkResult
 * [ ] VkSampleCountFlagBits
 * [ ] VkSampleCountFlags
 * [ ] VkSampleLocationEXT
 * [ ] VkSampleLocationsInfoEXT
 * [ ] VkSampleMask
 * [ ] VkSampler
 * [ ] VkSamplerAddressMode
 * [ ] VkSamplerBlockMatchWindowCreateInfoQCOM
 * [ ] VkSamplerBorderColorComponentMappingCreateInfoEXT
 * [ ] VkSamplerCaptureDescriptorDataInfoEXT
 * [ ] VkSamplerCreateFlagBits
 * [ ] VkSamplerCreateFlags
 * [ ] VkSamplerCreateInfo
 * [ ] VkSamplerCubicWeightsCreateInfoQCOM
 * [ ] VkSamplerCustomBorderColorCreateInfoEXT
 * [ ] VkSamplerMipmapMode
 * [ ] VkSamplerReductionMode
 * [ ] VkSamplerReductionModeCreateInfo
 * [ ] VkSamplerReductionModeCreateInfoEXT
 * [ ] VkSamplerReductionModeEXT
 * [ ] VkSamplerYcbcrConversion
 * [ ] VkSamplerYcbcrConversionCreateInfo
 * [ ] VkSamplerYcbcrConversionCreateInfoKHR
 * [ ] VkSamplerYcbcrConversionImageFormatProperties
 * [ ] VkSamplerYcbcrConversionImageFormatPropertiesKHR
 * [ ] VkSamplerYcbcrConversionInfo
 * [ ] VkSamplerYcbcrConversionInfoKHR
 * [ ] VkSamplerYcbcrConversionKHR
 * [ ] VkSamplerYcbcrConversionYcbcrDegammaCreateInfoQCOM
 * [ ] VkSamplerYcbcrModelConversion
 * [ ] VkSamplerYcbcrModelConversionKHR
 * [ ] VkSamplerYcbcrRange
 * [ ] VkSamplerYcbcrRangeKHR
 * [ ] VkScopeKHR
 * [ ] VkScopeNV
 * [x] VkSemaphore
 * [ ] VkSemaphoreCreateFlags
 * [x] VkSemaphoreCreateInfo
 * [ ] VkSemaphoreGetFdInfoKHR
 * [ ] VkSemaphoreImportFlagBits
 * [ ] VkSemaphoreImportFlagBitsKHR
 * [ ] VkSemaphoreImportFlags
 * [ ] VkSemaphoreImportFlagsKHR
 * [ ] VkSemaphoreSignalInfo
 * [ ] VkSemaphoreSignalInfoKHR
 * [ ] VkSemaphoreSubmitInfo
 * [ ] VkSemaphoreSubmitInfoKHR
 * [ ] VkSemaphoreType
 * [ ] VkSemaphoreTypeCreateInfo
 * [ ] VkSemaphoreTypeCreateInfoKHR
 * [ ] VkSemaphoreTypeKHR
 * [ ] VkSemaphoreWaitFlagBits
 * [ ] VkSemaphoreWaitFlagBitsKHR
 * [ ] VkSemaphoreWaitFlags
 * [ ] VkSemaphoreWaitFlagsKHR
 * [ ] VkSemaphoreWaitInfo
 * [ ] VkSemaphoreWaitInfoKHR
 * [ ] VkSetDescriptorBufferOffsetsInfoEXT
 * [ ] VkSetLatencyMarkerInfoNV
 * [ ] VkSetStateFlagsIndirectCommandNV
 * [ ] VkShaderCodeTypeEXT
 * [ ] VkShaderCorePropertiesFlagBitsAMD
 * [ ] VkShaderCorePropertiesFlagsAMD
 * [ ] VkShaderCreateFlagBitsEXT
 * [ ] VkShaderCreateFlagsEXT
 * [ ] VkShaderCreateInfoEXT
 * [ ] VkShaderEXT
 * [ ] VkShaderFloatControlsIndependence
 * [ ] VkShaderFloatControlsIndependenceKHR
 * [ ] VkShaderGroupShaderKHR
 * [ ] VkShaderInfoTypeAMD
 * [x] VkShaderModule
 * [ ] VkShaderModuleCreateFlags
 * [x] VkShaderModuleCreateInfo
 * [ ] VkShaderModuleIdentifierEXT
 * [ ] VkShaderModuleValidationCacheCreateInfoEXT
 * [ ] VkShaderRequiredSubgroupSizeCreateInfoEXT
 * [ ] VkShaderResourceUsageAMD
 * [x] VkShaderStageFlagBits
 * [ ] VkShaderStageFlags
 * [ ] VkShaderStatisticsInfoAMD
 * [ ] VkShadingRatePaletteEntryNV
 * [ ] VkShadingRatePaletteNV
 * [ ] VkSharedPresentSurfaceCapabilitiesKHR
 * [ ] VkSharingMode
 * [ ] VkSparseBufferMemoryBindInfo
 * [ ] VkSparseImageFormatFlagBits
 * [ ] VkSparseImageFormatFlags
 * [ ] VkSparseImageFormatProperties
 * [ ] VkSparseImageFormatProperties2
 * [ ] VkSparseImageFormatProperties2KHR
 * [ ] VkSparseImageMemoryBind
 * [ ] VkSparseImageMemoryBindInfo
 * [ ] VkSparseImageMemoryRequirements
 * [ ] VkSparseImageMemoryRequirements2
 * [ ] VkSparseImageMemoryRequirements2KHR
 * [ ] VkSparseImageOpaqueMemoryBindInfo
 * [ ] VkSparseMemoryBind
 * [ ] VkSparseMemoryBindFlagBits
 * [ ] VkSparseMemoryBindFlags
 * [ ] VkSpecializationInfo
 * [ ] VkSpecializationMapEntry
 * [ ] VkSRTDataNV
 * [ ] VkStencilFaceFlagBits
 * [ ] VkStencilFaceFlags
 * [ ] VkStencilOp
 * [ ] VkStencilOpState
 * [ ] VkStridedDeviceAddressRegionKHR
 * [ ] VkStructureType
 * [ ] VkSubgroupFeatureFlagBits
 * [ ] VkSubgroupFeatureFlags
 * [ ] VkSubmitFlagBits
 * [ ] VkSubmitFlagBitsKHR
 * [ ] VkSubmitFlags
 * [ ] VkSubmitFlagsKHR
 * [x] VkSubmitInfo
 * [ ] VkSubmitInfo2
 * [ ] VkSubmitInfo2KHR
 * [ ] VkSubpassBeginInfo
 * [ ] VkSubpassBeginInfoKHR
 * [ ] VkSubpassContents
 * [x] VkSubpassDependency
 * [ ] VkSubpassDependency2
 * [ ] VkSubpassDependency2KHR
 * [x] VkSubpassDescription
 * [ ] VkSubpassDescription2
 * [ ] VkSubpassDescription2KHR
 * [ ] VkSubpassDescriptionDepthStencilResolve
 * [ ] VkSubpassDescriptionDepthStencilResolveKHR
 * [ ] VkSubpassDescriptionFlagBits
 * [ ] VkSubpassDescriptionFlags
 * [ ] VkSubpassEndInfo
 * [ ] VkSubpassEndInfoKHR
 * [ ] VkSubpassFragmentDensityMapOffsetEndInfoQCOM
 * [ ] VkSubpassMergeStatusEXT
 * [ ] VkSubpassResolvePerformanceQueryEXT
 * [ ] VkSubpassSampleLocationsEXT
 * [ ] VkSubpassShadingPipelineCreateInfoHUAWEI
 * [ ] VkSubresourceHostMemcpySizeEXT
 * [ ] VkSubresourceLayout
 * [ ] VkSubresourceLayout2EXT
 * [ ] VkSubresourceLayout2KHR
 * [ ] VkSurfaceCapabilities2EXT
 * [ ] VkSurfaceCapabilities2KHR
 * [x] VkSurfaceCapabilitiesKHR
 * [ ] VkSurfaceCapabilitiesPresentBarrierNV
 * [ ] VkSurfaceCounterFlagBitsEXT
 * [ ] VkSurfaceCounterFlagsEXT
 * [ ] VkSurfaceFormat2KHR
 * [x] VkSurfaceFormatKHR
 * [x] VkSurfaceKHR
 * [ ] VkSurfacePresentModeCompatibilityEXT
 * [ ] VkSurfacePresentModeEXT
 * [ ] VkSurfacePresentScalingCapabilitiesEXT
 * [ ] VkSurfaceProtectedCapabilitiesKHR
 * [ ] VkSurfaceTransformFlagBitsKHR
 * [ ] VkSurfaceTransformFlagsKHR
 * [ ] VkSwapchainCounterCreateInfoEXT
 * [ ] VkSwapchainCreateFlagBitsKHR
 * [ ] VkSwapchainCreateFlagsKHR
 * [x] VkSwapchainCreateInfoKHR
 * [ ] VkSwapchainDisplayNativeHdrCreateInfoAMD
 * [x] VkSwapchainKHR
 * [ ] VkSwapchainLatencyCreateInfoNV
 * [ ] VkSwapchainPresentBarrierCreateInfoNV
 * [ ] VkSwapchainPresentFenceInfoEXT
 * [ ] VkSwapchainPresentModeInfoEXT
 * [ ] VkSwapchainPresentModesCreateInfoEXT
 * [ ] VkSwapchainPresentScalingCreateInfoEXT
 * [ ] VkSystemAllocationScope
 * [ ] VkTessellationDomainOrigin
 * [ ] VkTessellationDomainOriginKHR
 * [ ] VkTextureLODGatherFormatPropertiesAMD
 * [ ] VkTilePropertiesQCOM
 * [ ] VkTimeDomainEXT
 * [ ] VkTimeDomainKHR
 * [ ] VkTimelineSemaphoreSubmitInfo
 * [ ] VkTimelineSemaphoreSubmitInfoKHR
 * [ ] VkToolPurposeFlagBits
 * [ ] VkToolPurposeFlagBitsEXT
 * [ ] VkToolPurposeFlags
 * [ ] VkToolPurposeFlagsEXT
 * [ ] VkTraceRaysIndirectCommand2KHR
 * [ ] VkTraceRaysIndirectCommandKHR
 * [ ] VkTransformMatrixKHR
 * [ ] VkTransformMatrixNV
 * [ ] VkValidationCacheCreateFlagsEXT
 * [ ] VkValidationCacheCreateInfoEXT
 * [ ] VkValidationCacheEXT
 * [ ] VkValidationCacheHeaderVersionEXT
 * [ ] VkValidationCheckEXT
 * [ ] VkValidationFeatureDisableEXT
 * [ ] VkValidationFeatureEnableEXT
 * [ ] VkValidationFeaturesEXT
 * [ ] VkValidationFlagsEXT
 * [ ] VkVendorId
 * [x] VkVertexInputAttributeDescription
 * [ ] VkVertexInputAttributeDescription2EXT
 * [x] VkVertexInputBindingDescription
 * [ ] VkVertexInputBindingDescription2EXT
 * [ ] VkVertexInputBindingDivisorDescriptionEXT
 * [ ] VkVertexInputBindingDivisorDescriptionKHR
 * [ ] VkVertexInputRate
 * [ ] VkVideoBeginCodingFlagsKHR
 * [ ] VkVideoBeginCodingInfoKHR
 * [ ] VkVideoCapabilitiesKHR
 * [ ] VkVideoCapabilityFlagBitsKHR
 * [ ] VkVideoCapabilityFlagsKHR
 * [ ] VkVideoChromaSubsamplingFlagBitsKHR
 * [ ] VkVideoChromaSubsamplingFlagsKHR
 * [ ] VkVideoCodecOperationFlagBitsKHR
 * [ ] VkVideoCodecOperationFlagsKHR
 * [ ] VkVideoCodingControlFlagBitsKHR
 * [ ] VkVideoCodingControlFlagsKHR
 * [ ] VkVideoCodingControlInfoKHR
 * [ ] VkVideoComponentBitDepthFlagBitsKHR
 * [ ] VkVideoComponentBitDepthFlagsKHR
 * [ ] VkVideoDecodeCapabilitiesKHR
 * [ ] VkVideoDecodeCapabilityFlagBitsKHR
 * [ ] VkVideoDecodeCapabilityFlagsKHR
 * [ ] VkVideoDecodeFlagsKHR
 * [ ] VkVideoDecodeH264CapabilitiesKHR
 * [ ] VkVideoDecodeH264DpbSlotInfoKHR
 * [ ] VkVideoDecodeH264PictureInfoKHR
 * [ ] VkVideoDecodeH264PictureLayoutFlagBitsKHR
 * [ ] VkVideoDecodeH264PictureLayoutFlagsKHR
 * [ ] VkVideoDecodeH264ProfileInfoKHR
 * [ ] VkVideoDecodeH264SessionParametersAddInfoKHR
 * [ ] VkVideoDecodeH264SessionParametersCreateInfoKHR
 * [ ] VkVideoDecodeH265CapabilitiesKHR
 * [ ] VkVideoDecodeH265DpbSlotInfoKHR
 * [ ] VkVideoDecodeH265PictureInfoKHR
 * [ ] VkVideoDecodeH265ProfileInfoKHR
 * [ ] VkVideoDecodeH265SessionParametersAddInfoKHR
 * [ ] VkVideoDecodeH265SessionParametersCreateInfoKHR
 * [ ] VkVideoDecodeInfoKHR
 * [ ] VkVideoDecodeUsageFlagBitsKHR
 * [ ] VkVideoDecodeUsageFlagsKHR
 * [ ] VkVideoDecodeUsageInfoKHR
 * [ ] VkVideoEncodeCapabilitiesKHR
 * [ ] VkVideoEncodeCapabilityFlagBitsKHR
 * [ ] VkVideoEncodeCapabilityFlagsKHR
 * [ ] VkVideoEncodeContentFlagBitsKHR
 * [ ] VkVideoEncodeContentFlagsKHR
 * [ ] VkVideoEncodeFeedbackFlagBitsKHR
 * [ ] VkVideoEncodeFeedbackFlagsKHR
 * [ ] VkVideoEncodeFlagsKHR
 * [ ] VkVideoEncodeH264CapabilitiesKHR
 * [ ] VkVideoEncodeH264CapabilityFlagBitsKHR
 * [ ] VkVideoEncodeH264CapabilityFlagsKHR
 * [ ] VkVideoEncodeH264DpbSlotInfoKHR
 * [ ] VkVideoEncodeH264FrameSizeKHR
 * [ ] VkVideoEncodeH264GopRemainingFrameInfoKHR
 * [ ] VkVideoEncodeH264NaluSliceInfoKHR
 * [ ] VkVideoEncodeH264PictureInfoKHR
 * [ ] VkVideoEncodeH264ProfileInfoKHR
 * [ ] VkVideoEncodeH264QpKHR
 * [ ] VkVideoEncodeH264QualityLevelPropertiesKHR
 * [ ] VkVideoEncodeH264RateControlFlagBitsKHR
 * [ ] VkVideoEncodeH264RateControlFlagsKHR
 * [ ] VkVideoEncodeH264RateControlInfoKHR
 * [ ] VkVideoEncodeH264RateControlLayerInfoKHR
 * [ ] VkVideoEncodeH264SessionCreateInfoKHR
 * [ ] VkVideoEncodeH264SessionParametersAddInfoKHR
 * [ ] VkVideoEncodeH264SessionParametersCreateInfoKHR
 * [ ] VkVideoEncodeH264SessionParametersFeedbackInfoKHR
 * [ ] VkVideoEncodeH264SessionParametersGetInfoKHR
 * [ ] VkVideoEncodeH264StdFlagBitsKHR
 * [ ] VkVideoEncodeH264StdFlagsKHR
 * [ ] VkVideoEncodeH265CapabilitiesKHR
 * [ ] VkVideoEncodeH265CapabilityFlagBitsKHR
 * [ ] VkVideoEncodeH265CapabilityFlagsKHR
 * [ ] VkVideoEncodeH265CtbSizeFlagBitsKHR
 * [ ] VkVideoEncodeH265CtbSizeFlagsKHR
 * [ ] VkVideoEncodeH265DpbSlotInfoKHR
 * [ ] VkVideoEncodeH265FrameSizeKHR
 * [ ] VkVideoEncodeH265GopRemainingFrameInfoKHR
 * [ ] VkVideoEncodeH265NaluSliceSegmentInfoKHR
 * [ ] VkVideoEncodeH265PictureInfoKHR
 * [ ] VkVideoEncodeH265ProfileInfoKHR
 * [ ] VkVideoEncodeH265QpKHR
 * [ ] VkVideoEncodeH265QualityLevelPropertiesKHR
 * [ ] VkVideoEncodeH265RateControlFlagBitsKHR
 * [ ] VkVideoEncodeH265RateControlFlagsKHR
 * [ ] VkVideoEncodeH265RateControlInfoKHR
 * [ ] VkVideoEncodeH265RateControlLayerInfoKHR
 * [ ] VkVideoEncodeH265SessionCreateInfoKHR
 * [ ] VkVideoEncodeH265SessionParametersAddInfoKHR
 * [ ] VkVideoEncodeH265SessionParametersCreateInfoKHR
 * [ ] VkVideoEncodeH265SessionParametersFeedbackInfoKHR
 * [ ] VkVideoEncodeH265SessionParametersGetInfoKHR
 * [ ] VkVideoEncodeH265StdFlagBitsKHR
 * [ ] VkVideoEncodeH265StdFlagsKHR
 * [ ] VkVideoEncodeH265TransformBlockSizeFlagBitsKHR
 * [ ] VkVideoEncodeH265TransformBlockSizeFlagsKHR
 * [ ] VkVideoEncodeInfoKHR
 * [ ] VkVideoEncodeQualityLevelInfoKHR
 * [ ] VkVideoEncodeQualityLevelPropertiesKHR
 * [ ] VkVideoEncodeRateControlFlagsKHR
 * [ ] VkVideoEncodeRateControlInfoKHR
 * [ ] VkVideoEncodeRateControlLayerInfoKHR
 * [ ] VkVideoEncodeRateControlModeFlagBitsKHR
 * [ ] VkVideoEncodeRateControlModeFlagsKHR
 * [ ] VkVideoEncodeSessionParametersFeedbackInfoKHR
 * [ ] VkVideoEncodeSessionParametersGetInfoKHR
 * [ ] VkVideoEncodeTuningModeKHR
 * [ ] VkVideoEncodeUsageFlagBitsKHR
 * [ ] VkVideoEncodeUsageFlagsKHR
 * [ ] VkVideoEncodeUsageInfoKHR
 * [ ] VkVideoEndCodingFlagsKHR
 * [ ] VkVideoEndCodingInfoKHR
 * [ ] VkVideoFormatPropertiesKHR
 * [ ] VkVideoInlineQueryInfoKHR
 * [ ] VkVideoPictureResourceInfoKHR
 * [ ] VkVideoProfileInfoKHR
 * [ ] VkVideoProfileListInfoKHR
 * [ ] VkVideoReferenceSlotInfoKHR
 * [ ] VkVideoSessionCreateFlagBitsKHR
 * [ ] VkVideoSessionCreateFlagsKHR
 * [ ] VkVideoSessionCreateInfoKHR
 * [ ] VkVideoSessionKHR
 * [ ] VkVideoSessionMemoryRequirementsKHR
 * [ ] VkVideoSessionParametersCreateFlagsKHR
 * [ ] VkVideoSessionParametersCreateInfoKHR
 * [ ] VkVideoSessionParametersKHR
 * [ ] VkVideoSessionParametersUpdateInfoKHR
 * [x] VkViewport
 * [ ] VkViewportCoordinateSwizzleNV
 * [ ] VkViewportSwizzleNV
 * [ ] VkViewportWScalingNV
 * [ ] VkWriteDescriptorSet
 * [ ] VkWriteDescriptorSetAccelerationStructureKHR
 * [ ] VkWriteDescriptorSetAccelerationStructureNV
 * [ ] VkWriteDescriptorSetInlineUniformBlock
 * [ ] VkWriteDescriptorSetInlineUniformBlockEXT
 * [ ] VkXYColorEXT
 * ========================================================================== */
