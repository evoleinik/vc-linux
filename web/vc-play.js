// Only published command lines may become guest input. URL text selects an
// own map key; even Object.prototype names are unknown programs.
export function createProgramLauncher(programs, slug, done = () => {}) {
  const lines = Object.hasOwn(programs, slug) ? programs[slug] : [];
  let next = 0;
  let idleReads = 0;
  return {
    // Called only by readInput, never by a clock or a render callback.
    poll(queuedBytes) {
      idleReads++;
      if (queuedBytes || idleReads < 2 || next === lines.length) return null;
      idleReads = 0;
      const line = `${lines[next++]}\r`;
      if (next === lines.length) done();
      return line;
    },
    consumed(count) {
      // Reset even for user input or a partial read of a queued command.
      if (count) idleReads = 0;
    },
  };
}
