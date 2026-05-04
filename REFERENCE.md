# The Jam Programming Language Reference

**Version 0.1.0 — Working Draft**

This document specifies the syntax and semantics of the Jam programming
language. It is intended as a normative reference for compiler implementors
and a precise specification for users. Examples are illustrative; the
authoritative grammar is the parser implementation in `src/parser.cpp`.

The reference is organized by language construct rather than by topic. Each
section gives an informal description, the formal syntax in EBNF-like
notation, and worked examples. Forward references are used liberally; readers
unfamiliar with a particular construct may consult the relevant section
directly.

---

## Table of Contents

1. [Notation and Conventions](#1-notation-and-conventions)
2. [Lexical Structure](#2-lexical-structure) *(forthcoming)*
3. [Types](#3-types)
   1. [Scalar Types](#31-scalar-types) *(forthcoming)*
   2. [Aggregate Types](#32-aggregate-types) *(forthcoming)*
   3. [Reference Types](#33-reference-types)
4. [Bindings and Mutability](#4-bindings-and-mutability) *(forthcoming)*
5. [Expressions](#5-expressions) *(forthcoming)*
6. [Statements](#6-statements) *(forthcoming)*
7. [Functions](#7-functions) *(forthcoming)*
8. [Modules](#8-modules) *(forthcoming)*
9. [The Standard Library](#9-the-standard-library) *(forthcoming)*

---

## 1. Notation and Conventions

Throughout this reference, the following typographic conventions apply:

- `monospace` denotes literal source text — keywords, operators, and
  identifiers as they appear in a Jam program.
- *italic* denotes nonterminal symbols of the grammar.
- The metacharacters `[ ]`, `{ }`, and `|` follow standard EBNF usage:
  brackets enclose optional elements, braces enclose elements that may
  repeat zero or more times, and the bar separates alternatives.
- `T` is used as a placeholder for an arbitrary type unless otherwise
  noted. `N` is used as a placeholder for a non-negative integer literal.

Code samples are valid Jam source unless explicitly marked otherwise.

---

## 3. Types

The Jam type system is structural and second-class — types are not first-class
values, and there is no inheritance. The set of types is closed under a small
collection of constructors enumerated in this section.

### 3.3 Reference Types

A *reference type* describes a value that refers to memory owned elsewhere.
Jam has three reference-type families — single-item pointer, many-item
pointer, and slice. Pointer types require a *pointee mutability*
qualifier (`const` or `mut`); they share the same `*` introducer, with
an optional `[]` between the qualifier and the element type promoting
the pointer to many-item form.

Slices `[]T` and fixed-size arrays `[N]T` take no qualifier. Their
element mutability follows the binding that owns the storage: a
`var x: []u8` permits `x[i] = …` and a `const x: []u8` does not.

```
Type            ::=  PointerType | SliceType | ArrayType | BaseType | UserType
PointerType     ::=  '*' PtrMut [ '[' ']' ] Type
PtrMut          ::=  'const' | 'mut'
SliceType       ::=  '[' ']' Type
ArrayType       ::=  '[' N  ']' Type
```

There is no bare `*T` form: every pointer states its pointee mutability
explicitly. The keywords `const` and `mut` never *open* a type — `const`
appears as a binding qualifier elsewhere in the language, but `var`
never appears in a type at all. The only place `const` / `mut` arise in
type position is immediately after `*`.

#### 3.3.1 Single-Item Pointer

A *single-item pointer* `*const T` (read-only pointee) or `*mut T`
(writable pointee) refers to exactly one value of type `T`. Indexing is
not defined on a single-item pointer; to read or write the referent,
use the dereference suffix `.*`. A single-item pointer is obtained by
applying the `&` operator to an lvalue.

```jam
fn pointTo() u8 {
    var x: u8 = 42;
    var p: *mut u8 = &x;
    p.* = 100;      // store through pointer
    return x;        // 100
}

fn readOnly(p: *const u8) u8 {
    return p.*;     // read-only through pointer
}
```

#### 3.3.2 Many-Item Pointer

A *many-item pointer* `*const[] T` (read-only pointee) or `*mut[] T`
(writable pointee) refers to the first element of an unspecified-length
contiguous run of `T`s. Indexing with `p[i]` is defined and corresponds
to a pointer-arithmetic GEP. Many-item pointers are the natural
representation of decayed C arrays at the foreign-function boundary.

The `[]` between the qualifier and the element type is what promotes the
pointer from single-item to many-item; the syntax reads "pointer to
many `T`s, of `const`/`mut` mutability".

```jam
extern fn snprintf(buf: *mut[] u8, size: u64, format: *const[] u8, ...) i32;

fn fillFirstThree() u8 {
    var arr: [10]u8 = undefined;
    var p: *mut[] u8 = &arr[0];
    p[0] = 1; p[1] = 2; p[2] = 3;
    return arr[1];   // 2
}
```

For a collection of slices, prefer `[]*const T` / `[]*mut T` (a slice
of pointers, which carries a length) over composing many-item pointer
with slice element type. The latter is grammatically valid but rarely
the right shape; a length-bearing slice of pointers is almost always
what you actually want.

#### 3.3.3 Slice

A *slice* `[]T` is a `(pointer, length)` pair denoting a contiguous run of
`T`s of known length. Slices are first-class — they may be passed to
functions, returned from functions, and stored in locals. String literals
have type `[]u8` (and the alias `str`). Element mutability follows the
binding: a `var s: []u8` permits writes via `s[i] = …`; a `const s: []u8`
does not.

```jam
fn first(buf: []u8) u8 {
    return buf.ptr[0];          // .ptr projects the pointer half
}

fn lengthOf(buf: []u8) u64 {
    return buf.len;             // .len projects the length half
}

fn allocateFresh() []u32 {
    var data: []u32 = undefined;
    return data;
}
```

The two projections `.ptr` and `.len` decompose the slice into a
`[*]const T` (resp. `[*]var T`, depending on context) and a `u64`.

#### 3.3.4 Fixed-Size Array (Owned, Not a Reference)

A *fixed-size array* `[N]T` is a contiguous run of exactly `N` values of
type `T`, with `N` known at compile time. Unlike a slice, the length is
part of the type, and unlike a slice or pointer, the array is *owned*
data — the bytes live wherever the binding lives (stack, struct field,
global). Element mutability follows the binding: a `var arr: [10]u8`
permits writes to elements; a `const arr: [10]u8` does not.

```jam
fn makeBoard() u8 {
    var board: [10]u8 = undefined;       // mutable elements via `var` binding
    for i in 0:10 {
        board[i] = i;
    }
    return board[7];                      // 7
}

const Game = struct {
    score: u8,
    board: [10]u8,                        // owned by the struct
};
```

#### 3.3.5 Summary

| Form           | Family          | Example                                       |
| -------------- | --------------- | --------------------------------------------- |
| `*const T`     | single-item ptr | `fn f(p: *const u8) u8 { return p.*; }`       |
| `*mut T`       | single-item ptr | `var p: *mut u8 = &x; p.* = 5;`               |
| `*const[] T`   | many-item ptr   | `fn f(p: *const[] u8) u8 { return p[0]; }`    |
| `*mut[] T`     | many-item ptr   | `var p: *mut[] u8 = &arr[0]; p[0] = 1;`       |
| `[]T`          | slice           | `var s: []u32 = undefined;`                   |
| `[]*const T`   | slice of ptrs   | `fn f(rows: []*const u8) u8 { ... }`          |
| `[]*mut T`     | slice of ptrs   | `fn f(rows: []*mut u8) { rows[0].* = 0; }`    |
| `[N]T`         | owned array     | `var b: [10]u8 = undefined;`                  |

#### 3.3.6 Grammar Properties

**Unambiguity.** Every input string `Type` accepts at most one parse
tree. Each terminal that may begin a `Type` production (`*`, `[`, a
base-type keyword, or an identifier) selects exactly one rule, and
within each rule the remaining tokens are consumed deterministically.
The keywords `var`, `const`, and `mut` never *open* a type — `const`
and `mut` appear in type position only as the obligatory qualifier
after `*`, never as alternative openers, and `var` does not appear in a
type at all.

**LL(2) as written; LL(1) after left-factoring.** The form in §3.3 has
two places that need two tokens of lookahead:

1. After `*`, the parser must decide whether the optional `[]` (many-item
   marker) is present, but `[` could also be the opening of a fixed-size
   array `[N]` in the element type. The two cases are distinguished by
   the second token: `[ ]` is the many-item marker; `[ NUMBER` is part
   of the element type.
2. `[]T` (slice) and `[N]T` (fixed array) both start with `[`; the
   second token (`]` vs. number-literal) selects between them.

Both can be eliminated by a one-step left-factoring:

```
Type         ::= PointerType | BracketType | BaseType | UserType
PointerType  ::= '*' PtrMut PtrTail
PtrTail      ::= '[' ']' Type    (* many-item pointer *)
             |   Type            (* single-item pointer *)
PtrMut       ::= 'const' | 'mut'
BracketType  ::= '[' BracketBody
BracketBody  ::= ']' Type        (* slice *)
             |   Number ']' Type (* fixed-size array *)
```

Now every alternative is selected by a unique single-token lookahead:

| Nonterminal   | Alternatives & their `FIRST` sets                                      |
| ------------- | ---------------------------------------------------------------------- |
| `Type`        | `{ * }`, `{ [ }`, `{ u8, i8, …, bool, str, … }`, `{ ⟨identifier⟩ }`    |
| `PtrMut`      | `{ const }`, `{ mut }`                                                 |
| `PtrTail`     | `{ [ }` … but this is the conflict point — see below                   |
| `BracketBody` | `{ ] }`, `{ ⟨number-literal⟩ }`                                        |

`PtrTail` would not be LL(1) on its own: both alternatives can begin
with `[`. The parser resolves this by peeking *one* token past the `[`
to test whether it is `]` (many marker) or a number (start of a
`[N]T` element type). After this single-token check, the rest of the
grammar is LL(1) and parses without backtracking.

**Implementation.** The recursive-descent parser in `src/parser.cpp`
matches the factored form. After `*` and the required `const`/`mut`,
it tentatively consumes `[`, and if the next token is `]` it commits to
the many-item form; otherwise it rewinds the one consumed `[` and
parses the `[…]T` as the element type.

---

*Sections 1, 2, 3.1, 3.2, and 4–9 are forthcoming. The current draft covers
only reference-type syntax and semantics, which were promoted to a published
state ahead of the rest of the document by request.*
