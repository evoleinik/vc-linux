import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

// Load the page's ES module without adding a package-wide JS module policy.
const source = await readFile(new URL('../web/speaker.js', import.meta.url), 'utf8');
const { createSpeaker } = await import(`data:text/javascript,${encodeURIComponent(source)}`);
const events = [];
let created = 0;
class AudioContext {
  constructor() {
    created++;
    this.currentTime = 7;
    this.state = 'suspended';
    this.destination = 'output';
  }
  createOscillator() {
    return {
      type: 'sine',
      frequency: { setValueAtTime: (hz, time) => events.push(['frequency', hz, time]) },
      connect: () => {},
      start() { events.push(['start', this.type]); },
    };
  }
  createGain() {
    return {
      gain: { value: 1, setValueAtTime: (value, time) => events.push(['gain', value, time]) },
      connect: (target) => assert.equal(target, 'output'),
    };
  }
  resume() { this.state = 'running'; return Promise.resolve(); }
}

const speaker = createSpeaker(AudioContext);
speaker.setFrequency(440);
assert.equal(created, 0, 'loading the page / receiving a note never opens audio');
speaker.unlock();
assert.equal(created, 1, 'the first key gesture opens audio');
assert.deepEqual(events.slice(0, 3), [['start', 'square'], ['frequency', 440, 7], ['gain', 0.04, 7]]);
speaker.unlock();
assert.equal(created, 1, 'later keys reuse one oscillator/context');
speaker.setFrequency(262);
assert.deepEqual(events.slice(-2), [['frequency', 262, 7], ['gain', 0.04, 7]]);
speaker.setFrequency(0);
assert.deepEqual(events.at(-1), ['gain', 0, 7], 'PIT gate-off is silent');
speaker.setFrequency(440);
speaker.silence();
assert.deepEqual(events.at(-1), ['gain', 0, 7], 'quit mutes the oscillator');
assert.doesNotThrow(() => createSpeaker(null).unlock(), 'no audio API is harmless');
assert.doesNotThrow(() => createSpeaker(class { constructor() { throw Error('blocked'); } }).unlock());
const brokenAudio = createSpeaker(class { createOscillator() { throw Error('no audio device'); } });
brokenAudio.unlock();
assert.doesNotThrow(() => brokenAudio.setFrequency(440), 'a partially initialized device remains optional');
console.log('web speaker: gesture gate, square wave, frequencies and silence passed');
