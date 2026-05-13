// Prism.js language definition for Jam.
//
// Token alphabet is derived from tree-sitter-jam's queries/highlights.scm
// so the two stay in sync. When a token is added to the tree-sitter
// grammar, mirror it here.

(function () {
    if (typeof Prism === 'undefined') return;

    Prism.languages.jam = {
        // Line comments first so `//` doesn't get eaten as division.
        'comment': /\/\/.*/,

        // String literals with escape sequences:
        //   \n \r \t \\ \" \' \0     - simple escapes
        //   \xHH                      - hex byte
        //   \u{HHHHHH}                - Unicode codepoint
        'string': {
            pattern: /"(?:\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]{1,6}\}|.)|[^"\\])*"/,
            greedy: true,
            inside: {
                'escape': /\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]{1,6}\}|.)/
            }
        },

        // Comptime intrinsics — @sizeOf, @alignOf, etc.
        'intrinsic': /@[a-zA-Z_]\w*/,

        // Numbers: decimal, hex (0x), binary (0b), octal (0o),
        // optional fractional/exponent.
        'number': /\b(?:0x[0-9a-fA-F][0-9a-fA-F_]*(?:\.[0-9a-fA-F_]+(?:[pP][+-]?\d+)?)?|0b[01][01_]*|0o[0-7][0-7_]*|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)\b/,

        // true/false are constants — colored as literals.
        'boolean': /\b(?:true|false)\b/,

        // Reserved words from highlights.scm (plus `and`/`or`).
        'keyword': /\b(?:fn|tfn|const|var|if|else|while|for|in|return|break|continue|match|import|extern|export|pub|as|struct|union|enum|mut|move|and|or)\b/,

        // Built-in primitive types and the special-cased Self type.
        'builtin': /\b(?:u1|u8|u16|u32|u64|usize|i8|i16|i32|i64|isize|f32|f64|bool|str|type|Self)\b/,

        // Identifier directly followed by `(` is rendered as a callable.
        // Keywords already matched above, so this won't catch `fn(`.
        'function': /\b[a-zA-Z_]\w*(?=\s*\()/,

        // Operators from highlights.scm.
        'operator': /\.\.=|<<|>>|==|!=|<=|>=|[+\-*%&|^~<>=!]/,

        'punctuation': /[()[\]{},;:.]/
    };
})();
