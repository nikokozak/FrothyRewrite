export interface SourceForm {
  source: string;
  startOffset: number;
  endOffset: number;
  startLine: number;
  endLine: number;
  complete: boolean;
}

export interface TextDocumentLike {
  getText(): string;
  lineCount: number;
}

interface FormState {
  lines: string[];
  codeLines: string[];
  parenDepth: number;
  bracketDepth: number;
  braceDepth: number;
  inString: boolean;
  escaped: boolean;
  inBlockComment: boolean;
  startOffset: number;
  endOffset: number;
  startLine: number;
  endLine: number;
}

// ponytail: This mirror stops at selecting source spans. Replace it if the
// compiler/session exposes spans; do not grow it into a second parser.
export function splitForms(text: string): SourceForm[] {
  const forms: SourceForm[] = [];
  const state = emptyState();
  let lineStart = 0;
  let lineNumber = 0;

  while (lineStart < text.length) {
    const newline = text.indexOf('\n', lineStart);
    const rawEnd = newline < 0 ? text.length : newline;
    const lineEnd = rawEnd > lineStart && text[rawEnd - 1] === '\r' ? rawEnd - 1 : rawEnd;
    const form = appendLine(
      state,
      text.slice(lineStart, lineEnd),
      lineStart,
      lineEnd,
      lineNumber,
    );
    if (form) forms.push(form);
    if (newline < 0) break;
    lineStart = newline + 1;
    lineNumber++;
  }

  if (state.lines.length > 0) forms.push(formFrom(state, false));
  return forms;
}

export function formAt(document: TextDocumentLike, line: number): SourceForm | undefined {
  if (!Number.isInteger(line) || line < 0 || line >= document.lineCount) return undefined;
  const forms = splitForms(document.getText());
  const containing = forms.find((form) => line >= form.startLine && line <= form.endLine);
  if (containing) return containing;
  return forms.find((form) => form.startLine > line) ?? forms[forms.length - 1];
}

function appendLine(
  state: FormState,
  line: string,
  lineStart: number,
  lineEnd: number,
  lineNumber: number,
): SourceForm | undefined {
  const code = scanLine(state, line).trim();
  if (state.lines.length === 0 && code === '') return undefined;

  if (state.lines.length === 0) {
    state.startOffset = lineStart;
    state.startLine = lineNumber;
  }
  state.lines.push(line.trim());
  state.codeLines.push(code);
  state.endOffset = lineEnd;
  state.endLine = lineNumber;

  const codeSource = state.codeLines.filter(Boolean).join(' ');
  if (state.parenDepth !== 0 || state.bracketDepth !== 0 || state.braceDepth !== 0 ||
      state.inString || state.inBlockComment || sourceNeedsContinuation(codeSource)) {
    return undefined;
  }

  const form = formFrom(state, true);
  resetState(state);
  return form;
}

function scanLine(state: FormState, line: string): string {
  let code = '';
  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (state.inBlockComment) {
      if (ch === '*' && line[i + 1] === '-') {
        state.inBlockComment = false;
        i++;
      }
      continue;
    }
    if (state.inString) {
      code += ch;
      if (state.escaped) {
        state.escaped = false;
      } else if (ch === '\\') {
        state.escaped = true;
      } else if (ch === '"') {
        state.inString = false;
      }
      continue;
    }
    if (ch === '-' && commentCanStart(line, i)) {
      if (line[i + 1] === '-') break;
      if (line[i + 1] === '*') {
        state.inBlockComment = true;
        i++;
        continue;
      }
    }

    switch (ch) {
      case '"': state.inString = true; break;
      case '(': state.parenDepth++; break;
      case ')': if (state.parenDepth > 0) state.parenDepth--; break;
      case '[': state.bracketDepth++; break;
      case ']': if (state.bracketDepth > 0) state.bracketDepth--; break;
      case '{': state.braceDepth++; break;
      case '}': if (state.braceDepth > 0) state.braceDepth--; break;
    }
    code += ch;
  }
  return code;
}

function sourceNeedsContinuation(source: string): boolean {
  const trimmed = source.trim();
  if (trimmed === '') return false;
  if (trimmed.endsWith(',') || trimmed.endsWith('->')) return true;
  const fields = trimmed.split(/\s+/);
  return ['else', 'fn', 'forever', 'if', 'is', 'repeat', 'set', 'to', 'with']
    .includes(fields[fields.length - 1]);
}

function commentCanStart(line: string, index: number): boolean {
  if (index === 0) return true;
  return ' \t\r\n[(:,;'.includes(line[index - 1]);
}

function formFrom(state: FormState, complete: boolean): SourceForm {
  return {
    source: state.lines.filter(Boolean).join('\n'),
    startOffset: state.startOffset,
    endOffset: state.endOffset,
    startLine: state.startLine,
    endLine: state.endLine,
    complete,
  };
}

function emptyState(): FormState {
  return {
    lines: [],
    codeLines: [],
    parenDepth: 0,
    bracketDepth: 0,
    braceDepth: 0,
    inString: false,
    escaped: false,
    inBlockComment: false,
    startOffset: 0,
    endOffset: 0,
    startLine: 0,
    endLine: 0,
  };
}

function resetState(state: FormState): void {
  Object.assign(state, emptyState());
}
