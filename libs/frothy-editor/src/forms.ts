export interface SourceForm {
  source: string;
  startLine: number;
  endLine: number;
  complete: boolean;
}

interface FormState {
  codeLines: string[];
  parenDepth: number;
  bracketDepth: number;
  braceDepth: number;
  inString: boolean;
  escaped: boolean;
  inBlockComment: boolean;
  startLine: number;
  endLine: number;
}

// ponytail: This mirror stops at selecting and joining source spans. Replace it
// if the compiler/session exposes spans; do not grow it into a second parser.
export function splitForms(text: string): SourceForm[] {
  const forms: SourceForm[] = [];
  const state = emptyState();
  const lines = text.split(/\r?\n/);
  for (let lineNumber = 0; lineNumber < lines.length; lineNumber += 1) {
    const form = appendLine(state, lines[lineNumber], lineNumber);
    if (form) forms.push(form);
  }
  if (state.codeLines.length > 0) forms.push(formFrom(state, false));
  return forms;
}

export function formAt(text: string, line: number): SourceForm | undefined {
  const lineCount = text.split(/\r?\n/).length;
  if (!Number.isInteger(line) || line < 0 || line >= lineCount) return undefined;
  const forms = splitForms(text);
  const containing = forms.find((form) => line >= form.startLine && line <= form.endLine);
  if (containing) return containing;
  return forms.find((form) => form.startLine > line) ?? forms[forms.length - 1];
}

function appendLine(state: FormState, line: string, lineNumber: number): SourceForm | undefined {
  const code = scanLine(state, line).trim();
  if (state.codeLines.length === 0 && code === "") return undefined;
  if (state.codeLines.length === 0) state.startLine = lineNumber;
  state.codeLines.push(code);
  state.endLine = lineNumber;

  const source = state.codeLines.filter(Boolean).join(" ");
  if (state.parenDepth !== 0 || state.bracketDepth !== 0 || state.braceDepth !== 0 ||
      state.inString || state.inBlockComment || sourceNeedsContinuation(source)) {
    return undefined;
  }

  const form = formFrom(state, true);
  resetState(state);
  return form;
}

function scanLine(state: FormState, line: string): string {
  let code = "";
  for (let index = 0; index < line.length; index += 1) {
    const ch = line[index];
    if (state.inBlockComment) {
      if (ch === "*" && line[index + 1] === "-") {
        state.inBlockComment = false;
        index += 1;
      }
      continue;
    }
    if (state.inString) {
      code += ch;
      if (state.escaped) {
        state.escaped = false;
      } else if (ch === "\\") {
        state.escaped = true;
      } else if (ch === '"') {
        state.inString = false;
      }
      continue;
    }
    if (ch === "-" && commentCanStart(line, index)) {
      if (line[index + 1] === "-") break;
      if (line[index + 1] === "*") {
        state.inBlockComment = true;
        index += 1;
        continue;
      }
    }

    switch (ch) {
      case '"': state.inString = true; break;
      case "(": state.parenDepth += 1; break;
      case ")": if (state.parenDepth > 0) state.parenDepth -= 1; break;
      case "[": state.bracketDepth += 1; break;
      case "]": if (state.bracketDepth > 0) state.bracketDepth -= 1; break;
      case "{": state.braceDepth += 1; break;
      case "}": if (state.braceDepth > 0) state.braceDepth -= 1; break;
    }
    code += ch;
  }
  return code;
}

function sourceNeedsContinuation(source: string): boolean {
  const trimmed = source.trim();
  if (trimmed === "") return false;
  if (trimmed.endsWith(",") || trimmed.endsWith("->")) return true;
  const fields = trimmed.split(/\s+/);
  return ["else", "fn", "forever", "if", "is", "repeat", "set", "to", "with"]
    .includes(fields[fields.length - 1]);
}

function commentCanStart(line: string, index: number): boolean {
  if (index === 0) return true;
  return " \t\r\n[(:,;".includes(line[index - 1]);
}

function formFrom(state: FormState, complete: boolean): SourceForm {
  return {
    source: state.codeLines.filter(Boolean).join(" "),
    startLine: state.startLine,
    endLine: state.endLine,
    complete,
  };
}

function emptyState(): FormState {
  return {
    codeLines: [],
    parenDepth: 0,
    bracketDepth: 0,
    braceDepth: 0,
    inString: false,
    escaped: false,
    inBlockComment: false,
    startLine: 0,
    endLine: 0,
  };
}

function resetState(state: FormState): void {
  Object.assign(state, emptyState());
}
