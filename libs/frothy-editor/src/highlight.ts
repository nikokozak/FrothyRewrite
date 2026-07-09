// Frothy stream tokenizer for CodeMirror 6.
//
// Grammar (small enough to fit in a stream tokenizer; a Lezer grammar
// is a follow-up):
//   keywords:  after and as attempt boot cancel cells changes debounce else
//              every falling fn forever here if is not on or record repeat
//              rescue rising set to unless when while with
//   constants: true false nil $device_constant
//   comments:  -- line comments, -* block comments *-
//   numbers:   decimal (`123`, `-123`), hex (`0xFF`), binary (`0b1010`),
//              percent (`50%`)
//   text:      "..." with \n \r \t \" \\ \xNN escapes
//   brackets:  [ ]
//   operators: + - * / % < > <= >= = == ,
//   names:     identifier chars (.: allowed inside, e.g. text.concat:)
//   call mark: trailing `:` on a name marks a function call site

import { StreamLanguage, LanguageSupport } from "@codemirror/language";
import { HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags as t } from "@lezer/highlight";

const KEYWORDS = new Set([
  "after",
  "and",
  "as",
  "attempt",
  "boot",
  "cancel",
  "cells",
  "changes",
  "debounce",
  "else",
  "every",
  "falling",
  "fn",
  "forever",
  "here",
  "if",
  "is",
  "not",
  "on",
  "or",
  "record",
  "repeat",
  "rescue",
  "rising",
  "set",
  "to",
  "unless",
  "when",
  "while",
  "with",
]);

const CONSTANTS = new Set(["true", "false", "nil"]);

interface FrothyState {
  inString: boolean;
  escape: boolean;
  inBlockComment: boolean;
  // True when whitespace or a line start precedes the current position. A `--`
  // / `-*` opens a comment only then — mirrors parse.c's `skipped` gate, so
  // `10--5` stays arithmetic (10 - -5) rather than being grayed as a comment.
  spaceBefore: boolean;
}

function startState(): FrothyState {
  return { inString: false, escape: false, inBlockComment: false, spaceBefore: true };
}

function isNameStart(ch: string): boolean {
  return /[A-Za-z_]/.test(ch);
}

function isNamePart(ch: string): boolean {
  return /[A-Za-z0-9_\-.]/.test(ch);
}

interface StreamHandle {
  eat(ch: string): boolean;
  peek(): string | null;
  next(): string | undefined;
  eatWhile(re: RegExp): boolean;
  eatSpace(): boolean;
  match(re: RegExp): RegExpMatchArray | boolean | null;
  current(): string;
  skipToEnd(): void;
  sol?(): boolean;
}

function tokenize(stream: StreamHandle, state: FrothyState): string | null {
  if (state.inBlockComment) {
    consumeBlockComment(stream, state);
    return "comment";
  }

  if (state.inString) {
    while (stream.peek() != null) {
      const ch = stream.next();
      if (state.escape) {
        state.escape = false;
        continue;
      }
      if (ch === "\\") {
        state.escape = true;
        continue;
      }
      if (ch === '"') {
        state.inString = false;
        return "string";
      }
    }
    return "string";
  }

  if (stream.sol?.()) state.spaceBefore = true;
  if (stream.eatSpace()) {
    state.spaceBefore = true;
    return null;
  }

  const ch = stream.peek();
  if (ch == null) return null;

  // Whether whitespace/line-start immediately precedes THIS token; consumed
  // here so every real token clears it for the next one.
  // ponytail: we skip parse.c's rarer comment-after-a-delimiter-without-space
  // case (`[--x]`); nobody writes that.
  const spaceBefore = state.spaceBefore;
  state.spaceBefore = false;

  if (ch === '"') {
    stream.next();
    state.inString = true;
    return "string";
  }

  if (ch === "-" && spaceBefore) {
    if (stream.match(/^--/)) {
      stream.skipToEnd();
      return "comment";
    }
    if (stream.match(/^-\*/)) {
      state.inBlockComment = true;
      consumeBlockComment(stream, state);
      return "comment";
    }
  }

  if (ch === "[" || ch === "]") {
    stream.next();
    return "bracket";
  }

  if (ch === "," || ch === ";") {
    stream.next();
    return "separator";
  }

  // Numbers: 0xHEX, 0bBIN, decimal, optional leading - on decimal and trailing %.
  if (
    stream.match(/^-[0-9]+/) ||
    stream.match(/^0x[0-9a-fA-F]+/) ||
    stream.match(/^0b[01]+/) ||
    stream.match(/^-?[0-9]+/)
  ) {
    stream.eat("%");
    return "number";
  }

  // Operators.
  if ("+-*/%<>=".indexOf(ch) >= 0) {
    stream.next();
    stream.eat("=");
    return "operator";
  }

  if (ch === "$" && stream.match(/^\$[A-Za-z_][A-Za-z0-9_\-.]*/)) {
    return "constant";
  }

  // Names — letters / digits / dots / dashes. A trailing `:` marks a call.
  if (isNameStart(ch)) {
    stream.next();
    while (true) {
      const next = stream.peek();
      if (next == null || !isNamePart(next)) break;
      stream.next();
    }
    const word = stream.current();
    if (KEYWORDS.has(word)) return "keyword";
    if (CONSTANTS.has(word)) return "constant";
    if (stream.eat(":")) return "callName";
    return "name";
  }

  // Anything else: consume one char and emit no token.
  stream.next();
  return null;
}

function consumeBlockComment(stream: StreamHandle, state: FrothyState): void {
  while (stream.peek() != null) {
    const ch = stream.next();
    if (ch === "*" && stream.peek() === "-") {
      stream.next();
      state.inBlockComment = false;
      return;
    }
  }
}

const frothyStream = StreamLanguage.define<FrothyState>({
  name: "frothy",
  startState,
  token(stream: unknown, state: FrothyState) {
    return tokenize(stream as StreamHandle, state);
  },
  tokenTable: {
    keyword: t.keyword,
    name: t.variableName,
    callName: t.function(t.variableName),
    constant: t.bool,
    comment: t.comment,
    number: t.number,
    string: t.string,
    bracket: t.bracket,
    operator: t.operator,
    separator: t.punctuation,
  },
});

const frothyHighlightStyle = HighlightStyle.define([
  { tag: t.keyword, color: "var(--frothy-hl-keyword, #a05a1a)" },
  { tag: t.function(t.variableName), color: "var(--frothy-hl-call, #2c4a6e)" },
  { tag: t.variableName, color: "var(--frothy-hl-name, #2c1a10)" },
  { tag: t.number, color: "var(--frothy-hl-number, #3f6b3a)" },
  { tag: t.string, color: "var(--frothy-hl-string, #6b3a47)" },
  { tag: t.comment, color: "var(--frothy-hl-comment, #8a7f66)" },
  { tag: t.bool, color: "var(--frothy-hl-constant, #7a3aa0)" },
  { tag: t.bracket, color: "var(--frothy-hl-bracket, #6e4a2e)", fontWeight: "bold" },
  { tag: t.operator, color: "var(--frothy-hl-operator, #a05a1a)" },
  { tag: t.punctuation, color: "var(--frothy-hl-punct, #6e4a2e)" },
]);

export function frothyLanguage(): LanguageSupport {
  return new LanguageSupport(frothyStream);
}

export function frothyHighlight() {
  return syntaxHighlighting(frothyHighlightStyle);
}

// Test-only export: tokenize a source string and return the token kinds
// in order, ignoring whitespace.
export function tokenStream(source: string): string[] {
  const kinds: string[] = [];
  const state = startState();
  for (const line of source.split("\n")) {
    let pos = 0;
    const stream: StreamHandle = {
      eat(target: string): boolean {
        if (line[pos] === target) {
          pos += 1;
          return true;
        }
        return false;
      },
      peek(): string | null {
        return pos < line.length ? line[pos]! : null;
      },
      next(): string | undefined {
        if (pos >= line.length) return undefined;
        const ch = line[pos]!;
        pos += 1;
        return ch;
      },
      eatWhile(re: RegExp): boolean {
        let ate = false;
        while (pos < line.length && re.test(line[pos]!)) {
          pos += 1;
          ate = true;
        }
        return ate;
      },
      eatSpace(): boolean {
        let ate = false;
        while (pos < line.length && /\s/.test(line[pos]!)) {
          pos += 1;
          ate = true;
        }
        return ate;
      },
      match(re: RegExp): RegExpMatchArray | null {
        const rest = line.slice(pos);
        const m = rest.match(re);
        if (m && m.index === 0) {
          pos += m[0].length;
          return m;
        }
        return null;
      },
      current(): string {
        return line.slice(start, pos);
      },
      skipToEnd(): void {
        pos = line.length;
      },
      sol(): boolean {
        return pos === 0;
      },
    };

    let start = pos;
    while (pos < line.length) {
      start = pos;
      const kind = tokenize(stream, state);
      if (kind) kinds.push(kind);
      else if (pos === start) pos += 1;
    }
  }
  return kinds;
}
