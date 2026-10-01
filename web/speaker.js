// One oscillator models PIT channel 2. No audio context is created before a
// key gesture; a missing/blocked audio device must never prevent VC starting.
export function createSpeaker(AudioContextClass = globalThis.AudioContext
    || globalThis.webkitAudioContext) {
  let context;
  let oscillator;
  let gain;
  let frequency = 0;

  function apply() {
    if (!context) return;
    if (frequency > 0) oscillator.frequency.setValueAtTime(frequency, context.currentTime);
    gain.gain.setValueAtTime(frequency > 0 ? 0.04 : 0, context.currentTime);
  }

  return {
    unlock() {
      if (!AudioContextClass) return;
      try {
        if (!context) {
          context = new AudioContextClass();
          oscillator = context.createOscillator();
          gain = context.createGain();
          oscillator.type = "square";
          gain.gain.value = 0;
          oscillator.connect(gain);
          gain.connect(context.destination);
          oscillator.start();
          apply();
        }
        if (context.state === "suspended") context.resume().catch(() => {});
      } catch {
        // Audio is optional, including on browsers which disable it by policy.
        try { context?.close?.()?.catch?.(() => {}); } catch { /* no device */ }
        context = oscillator = gain = undefined;
      }
    },
    setFrequency(hz) {
      frequency = Number.isFinite(hz) && hz > 0 ? hz : 0;
      apply();
    },
    silence() {
      frequency = 0;
      apply();
    },
  };
}
