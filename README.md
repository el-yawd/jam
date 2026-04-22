<!-- LOGO -->
<h1>
<p align="center">
  <img src="./miscellaneous/jam-mascot-01.png" alt="Jam Logo" width="256">
  <br>Jam Programming Language
</h1>
  <p align="center">
    <a href="#about">About</a>
    ·
    <a href="#install">Install</a>
    ·
    <a href="#contributing">Contributing</a>
    ·
    <a href="https://github.com/raphamorim/jam/blob/main/CHANGELOG.md">Changelog</a>
    ·
    <a href="https://github.com/sponsors/raphamorim">Sponsor</a>
  </p>
</p>

Jam is a programming language designed to provide developers with the control and performance characteristics of C while incorporating modern safety features to mitigate common programming errors.

Built to be memory safe, the language targets developers who require bare-metal performance and direct hardware access without sacrificing code reliability and maintainability.

```jam
const std = import("std"); // Standard library
const { assert } = import("test");
const { yetAnotherPrintFunction } = import("folder"); // ./folder/mod.jam

fn main() u8 {
    std.fmt.println("Hello, World!");
    std.thread.sleep(sumTwoValues(8, 5));

    yetAnotherPrintFunction("It's Jamming Time!");

    return 0;
}

fn sumTwoValues(firstVal: u8, secondVal: u8) u8 {
    return firstVal.wrappingAdd(secondVal);
}

tfn sumTwoValues() {
    assert(sumTwoValues(8, 5), 13);
    assert(sumTwoValues(255, 1), 255);
}
```

## Rules

Nothing here is written in stone forever, it's open to discussion and I believe that with time it will suffer updates.

- Easy and declarative to use full CPU potential.
- No ambiguous syntax.
    - Jam grammar was intentionally designed to be unambiguous and simple to parse. This is one of the explicit design goals of the language.
- Performance competitive.
    - The language should be capable to not necessarily be faster in every aspect but at least compete with Rust, Zig, Nim and other languages in the same tier.

## Install

Run `make install` or `make build` if you just want the compiler binary.

## Contributing

[Donate monthly](https://github.com/sponsors/raphamorim).

Jam follows some of Zig contribution rules.

#### Strict No LLM / No AI Policy

No LLMs for issues.

No LLMs for patches / pull requests.

No LLMs for comments on the bug tracker, including translation.

English is encouraged, but not required. You are welcome to post in your native language and rely on others to have their own translation tools of choice to interpret your words.

## v0.1.0

- [ ] Spec/Documentation website
- [x] bool
    - [x] tests
    - [x] docs
- [x] u8, u16, u32, u64, i8, i16, i32, i64
    - [x] tests
    - [x] docs
- [x] strings
    - [x] tests
    - [x] docs
- [ ] functions
    - [ ] tests
    - [ ] docs
- [ ] for/while
    - [ ] tests
    - [ ] docs
- [ ] Be cabaple to use C abi
    - [ ] tests
    - [ ] docs
- [ ] Stack memory
- [ ] Smart pointers (inspiration https://doc.rust-lang.org/book/ch15-00-smart-pointers.html , https://learn.microsoft.com/en-us/cpp/cpp/smart-pointers-modern-cpp?view=msvc-170)
- [ ] Allocators
    - [ ] Linear/Bump Allocators
        - Why?
            - Simply increment a pointer for each allocation
            - Extremely fast
            - Can only free all memory at once
            - Great for temporary data
    - [ ] Custom Allocators
        - Why?
        - [ ] Slab allocators (kernel memory management)
        - [ ] Segregated free lists
        - [ ] Buddy allocators
        - [ ] Tailored to specific workload patterns
- [ ] Decent Microsoft Windows support (self note)
- [ ] Capable of creating a TUI
- [ ] Cranelift
- [ ] spirv

## todo

- [ ] create tree-sitter-grammar
- [ ] migrate docs to a jam builder
- [ ] Memory safety (evaluate Rust lifecycles and zig allocators)
- [ ] C compatible (also allow to auto generate .h files)
- [ ] C++ compatible ? (need to validate it correctly)
- [ ] Bottle package manager
- [ ] WebAssembly target
- [ ] async spec.

## Code of Conduct

Differently than the Jam rules, this area is non negotiable.

People from around the world, of all backgrounds, genders, and experience levels are welcome and respected equally.

## Frequent Asked Questions

#### Why not move to an organization instead?

In the future Jam indeed will go to an org, but now is mostly me maintaining the language and I am considering test other options besides GitHub in the future, so I would like to wait a bit before start moving things around.

## References

- Chris Lattner et al., "MLIR: A Compiler Infrastructure for the End of Moore's Law", 2020. https://arxiv.org/abs/2002.11054

## Artwork

Jam logo/mascot was created by [Anthony Orozco](https://www.behance.net/ntnay).

## License

This software is distributed under the Apache License 2.0 with LLVM Exceptions.
