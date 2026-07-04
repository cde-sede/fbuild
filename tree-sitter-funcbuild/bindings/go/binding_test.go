package tree_sitter_funcbuild_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_funcbuild "github.com/tree-sitter/tree-sitter-funcbuild/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_funcbuild.Language())
	if language == nil {
		t.Errorf("Error loading Funcbuild grammar")
	}
}
