import XCTest
import SwiftTreeSitter
import TreeSitterFuncbuild

final class TreeSitterFuncbuildTests: XCTestCase {
    func testCanLoadGrammar() throws {
        let parser = Parser()
        let language = Language(language: tree_sitter_funcbuild())
        XCTAssertNoThrow(try parser.setLanguage(language),
                         "Error loading Funcbuild grammar")
    }
}
