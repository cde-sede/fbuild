// A shared object that is not a plugin: no fb::plugin::api / open.
auto unrelated(void) noexcept -> int {
	return 1;
}
