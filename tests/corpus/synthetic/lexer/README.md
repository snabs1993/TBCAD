# Lexer test files

Hand-made by the TBCAD project (MIT licensed, no third-party content). Regenerate with
`python3 libs/dxf/tools/make_lexer_corpus.py`. Each `.dxf` has an `.expected.txt` with the
expected group stream (`G line code type value`) followed by the expected warnings
(`W line code`); `libs/dxf/tests/lexer/corpus_tests.cpp` compares them. Strings are escaped
as `\xNN` for non-ASCII bytes. For binary DXF the "line" is the byte offset of the group.
