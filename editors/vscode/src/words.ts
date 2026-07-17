// Parse the device's `words` reply: an optional `words` echo line, whitespace-
// separated names, and an optional trailing `ok`.
export function parseWords(response: string): string[] {
  const lines = response.trim().split(/\r?\n/);
  if (lines[0]?.trim() === 'words') lines.shift();
  const words = lines.join(' ').split(/\s+/);
  if (words[words.length - 1] === 'ok') words.pop();
  return [...new Set(words.filter(Boolean))];
}
