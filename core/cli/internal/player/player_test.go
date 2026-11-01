package player

import (
	"errors"
	"io"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/mewkiz/flac"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/flac/meta"
)

// --- the conversion, on synthetic buffers ----------------------------------

func TestMonoIsDuplicated(t *testing.T) {
	c := newConverter(1, 16)
	got := c.stereo16([]int32{100, -200, 32767, -32768})
	want := []int16{100, 100, -200, -200, 32767, 32767, -32768, -32768}
	if len(got) != len(want) {
		t.Fatalf("len %d, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("sample %d = %d, want %d (all %v)", i, got[i], want[i], got)
		}
	}
}

func TestStereoKeepsChannelsAndMultichannelKeepsFrontPair(t *testing.T) {
	got := newConverter(2, 16).stereo16([]int32{1, 2, 3, 4})
	if got[0] != 1 || got[1] != 2 || got[2] != 3 || got[3] != 4 {
		t.Errorf("stereo = %v", got)
	}
	got = newConverter(6, 16).stereo16([]int32{1, 2, 9, 9, 9, 9, 3, 4, 9, 9, 9, 9})
	if len(got) != 4 || got[0] != 1 || got[1] != 2 || got[2] != 3 || got[3] != 4 {
		t.Errorf("5.1 = %v, want the front pair", got)
	}
}

func Test24To16IsDitheredWithinOneLSB(t *testing.T) {
	c := newConverter(1, 24)
	var sum float64
	const n = 20000
	for i := 0; i < n; i++ {
		s := int32((i*7919)%(1<<23)) - 1<<22
		out := int64(c.to16(s))
		exact := float64(s) / 256
		if d := math.Abs(float64(out) - exact); d > 1.5 {
			t.Fatalf("%d → %d, exact %.2f: off by %.2f LSB", s, out, exact, d)
		}
		sum += float64(out) - exact
	}
	if mean := sum / n; math.Abs(mean) > 0.05 {
		t.Errorf("mean error %.3f LSB: the dither is biased", mean)
	}
	// Full scale clamps rather than wraps.
	if v := c.to16(1<<23 - 1); v != 32767 {
		t.Errorf("24-bit full scale → %d, want 32767", v)
	}
	if v := c.to16(-1 << 23); v != -32768 {
		t.Errorf("24-bit negative full scale → %d, want -32768", v)
	}
	// Shallower than 16 shifts up.
	if v := newConverter(1, 8).to16(-128); v != -32768 {
		t.Errorf("8-bit -128 → %d, want -32768", v)
	}
}

func stereoRamp(n int, f func(i int) int16) []int16 {
	s := make([]int16, 0, 2*n)
	for i := 0; i < n; i++ {
		s = append(s, f(i), -f(i))
	}
	return s
}

func TestResamplerLengthAndShape(t *testing.T) {
	const in = 48000
	src := stereoRamp(in, func(i int) int16 { return int16(i % 30000) })
	out := newResampler(in).process(src)
	frames := len(out) / 2
	if frames < OutRate-2 || frames > OutRate+2 {
		t.Fatalf("one second at 48 kHz → %d frames, want ≈ %d", frames, OutRate)
	}
	// A ramp stays a ramp: each output is the input at i·48000/44100.
	for j := 0; j < 1000; j++ {
		want := float64(j) * in / OutRate
		if d := math.Abs(float64(out[2*j]) - want); d > 1 {
			t.Fatalf("frame %d = %d, want %.1f", j, out[2*j], want)
		}
		if out[2*j+1] != -out[2*j] {
			t.Fatalf("frame %d: right %d is not -left %d", j, out[2*j+1], out[2*j])
		}
	}
}

func TestResamplerChunkedEqualsWhole(t *testing.T) {
	for _, rate := range []int{22050, 32000, 48000, 96000} {
		src := stereoRamp(9000, func(i int) int16 { return int16(1000 * math.Sin(float64(i)/40)) })
		whole := newResampler(rate).process(src)
		r := newResampler(rate)
		var parts []int16
		for i := 0; i < len(src); {
			n := 2 * (37 + i%300) // odd-sized chunks, whole frames
			if i+n > len(src) {
				n = len(src) - i
			}
			parts = append(parts, r.process(src[i:i+n])...)
			i += n
		}
		if len(parts) != len(whole) {
			t.Fatalf("%d Hz: chunked %d samples, whole %d", rate, len(parts), len(whole))
		}
		for i := range whole {
			if parts[i] != whole[i] {
				t.Fatalf("%d Hz: sample %d chunked %d, whole %d", rate, i, parts[i], whole[i])
			}
		}
	}
}

func TestResamplerAt44100IsAPassThrough(t *testing.T) {
	if r := newResampler(OutRate); r != nil {
		t.Fatal("44.1 kHz should need no resampler")
	}
	in := []int16{1, 2, 3, 4}
	var r *resampler
	if got := r.process(in); &got[0] != &in[0] {
		t.Error("a nil resampler copied its input")
	}
}

// --- the player, on a fake device ------------------------------------------

// fakeVoice is a device stream that "plays" by draining its reader as fast
// as it can, with nothing buffered.
type fakeVoice struct {
	r       io.Reader
	mu      sync.Mutex
	playing bool
	closed  bool
	got     []byte
	vol     []float64
	started chan struct{}
	once    sync.Once
}

type fakeDevice struct{ voices chan *fakeVoice }

func (d *fakeDevice) newVoice(r io.Reader) voice {
	v := &fakeVoice{r: r, started: make(chan struct{})}
	d.voices <- v
	return v
}

func (v *fakeVoice) Play() {
	v.mu.Lock()
	if v.playing || v.closed {
		v.mu.Unlock()
		return
	}
	v.playing = true
	v.mu.Unlock()
	v.once.Do(func() { close(v.started) })
	go func() {
		buf := make([]byte, 4096)
		for {
			v.mu.Lock()
			if !v.playing || v.closed {
				v.mu.Unlock()
				return
			}
			v.mu.Unlock()
			n, err := v.r.Read(buf)
			v.mu.Lock()
			v.got = append(v.got, buf[:n]...)
			if err == io.EOF {
				v.playing = false
				v.mu.Unlock()
				return
			}
			v.mu.Unlock()
		}
	}()
}

func (v *fakeVoice) Pause()              { v.mu.Lock(); v.playing = false; v.mu.Unlock() }
func (v *fakeVoice) IsPlaying() bool     { v.mu.Lock(); defer v.mu.Unlock(); return v.playing }
func (v *fakeVoice) BufferedSize() int   { return 0 }
func (v *fakeVoice) SetVolume(x float64) { v.mu.Lock(); v.vol = append(v.vol, x); v.mu.Unlock() }
func (v *fakeVoice) Err() error          { return nil }
func (v *fakeVoice) Close() error        { v.mu.Lock(); v.closed = true; v.mu.Unlock(); return nil }

// writeFLAC encodes frames of verbatim samples: chans[c][i].
func writeFLAC(t *testing.T, path string, rate, bits int, chans [][]int32) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	n := len(chans[0])
	info := &meta.StreamInfo{BlockSizeMin: 4096, BlockSizeMax: 4096, SampleRate: uint32(rate),
		NChannels: uint8(len(chans)), BitsPerSample: uint8(bits), NSamples: uint64(n)}
	enc, err := flac.NewEncoder(f, info)
	if err != nil {
		t.Fatal(err)
	}
	layout := frame.ChannelsMono
	if len(chans) == 2 {
		layout = frame.ChannelsLR
	}
	for num, at := 0, 0; at < n; num, at = num+1, at+4096 {
		end := at + 4096
		if end > n {
			end = n
		}
		fr := &frame.Frame{Header: frame.Header{HasFixedBlockSize: true, BlockSize: uint16(end - at),
			SampleRate: uint32(rate), Channels: layout, BitsPerSample: uint8(bits), Num: uint64(num)}}
		for c := range chans {
			fr.Subframes = append(fr.Subframes, &frame.Subframe{
				SubHeader: frame.SubHeader{Pred: frame.PredVerbatim},
				Samples:   append([]int32(nil), chans[c][at:end]...), NSamples: end - at})
		}
		if err := enc.WriteFrame(fr); err != nil {
			t.Fatal(err)
		}
	}
	if err := enc.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestPlayToTheEndRunsOnEnd(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "tone.flac")
	// Half a second of 24-bit mono at 48 kHz: every conversion at once.
	const n = 24000
	mono := make([]int32, n)
	for i := range mono {
		mono[i] = int32(4_000_000 * math.Sin(float64(i)/20))
	}
	writeFLAC(t, path, 48000, 24, [][]int32{mono})

	dev := &fakeDevice{voices: make(chan *fakeVoice, 4)}
	p := newWith(dev)
	ended := make(chan struct{})
	p.OnEnd(func() { close(ended) })
	if err := p.Play(path); err != nil {
		t.Fatal(err)
	}
	v := <-dev.voices
	select {
	case <-ended:
	case <-time.After(5 * time.Second):
		t.Fatal("OnEnd never ran")
	}
	if p.State() != Stopped {
		t.Errorf("state after the end = %v, want stopped", p.State())
	}
	v.mu.Lock()
	got := v.got
	v.mu.Unlock()
	frames := len(got) / bytesPerFrame
	if want := n * OutRate / 48000; frames < want-4 || frames > want+4 {
		t.Errorf("played %d frames, want ≈ %d", frames, want)
	}
	for i := 0; i+3 < len(got); i += 4 {
		if got[i] != got[i+2] || got[i+1] != got[i+3] {
			t.Fatalf("frame %d: mono was not duplicated (% x)", i/4, got[i:i+4])
		}
	}
}

func TestPositionPauseAndSeek(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "long.flac")
	const n = 44100 * 3
	l, r := make([]int32, n), make([]int32, n)
	writeFLAC(t, path, 44100, 16, [][]int32{l, r})

	dev := &fakeDevice{voices: make(chan *fakeVoice, 4)}
	p := newWith(dev)
	if err := p.Play(path); err != nil {
		t.Fatal(err)
	}
	<-dev.voices
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, total := p.Position(); total == 3*time.Second {
			break
		}
		if time.Now().After(deadline) {
			_, total := p.Position()
			t.Fatalf("total = %v, want 3s", total)
		}
		time.Sleep(5 * time.Millisecond)
	}
	p.Pause()
	if p.State() != Paused {
		t.Fatalf("state = %v after Pause", p.State())
	}
	if err := p.Seek(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	<-dev.voices
	if p.State() != Paused {
		t.Errorf("a Seek while paused left the player %v", p.State())
	}
	if pos, _ := p.Position(); pos < 2*time.Second || pos > 2*time.Second+100*time.Millisecond {
		t.Errorf("position after Seek(2s) = %v", pos)
	}
	p.Stop()
	if pos, total := p.Position(); pos != 0 || total != 0 || p.State() != Stopped {
		t.Errorf("after Stop: %v / %v, %v", pos, total, p.State())
	}
}

func TestPlayRefusesWhatItCannotPlay(t *testing.T) {
	p := newWith(&fakeDevice{voices: make(chan *fakeVoice, 4)})
	dir := t.TempDir()
	if err := p.Play(filepath.Join(dir, "gone.flac")); !errors.Is(err, ErrMissing) || !errors.Is(err, os.ErrNotExist) {
		t.Errorf("a missing file: %v, want ErrMissing", err)
	}
	m4a := filepath.Join(dir, "x.m4a")
	_ = os.WriteFile(m4a, []byte("x"), 0o644)
	if err := p.Play(m4a); !errors.Is(err, ErrUnsupported) {
		t.Errorf("an .m4a: %v, want ErrUnsupported", err)
	}
	if err := p.Seek(time.Second); !errors.Is(err, ErrNoSeek) {
		t.Errorf("Seek with nothing loaded: %v, want ErrNoSeek", err)
	}
}

func TestADecodeErrorReachesOnError(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.flac")
	_ = os.WriteFile(bad, []byte("not a flac at all"), 0o644)
	dev := &fakeDevice{voices: make(chan *fakeVoice, 4)}
	p := newWith(dev)
	got := make(chan error, 1)
	p.OnError(func(err error) { got <- err })
	if err := p.Play(bad); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-got:
		if err == nil {
			t.Error("OnError(nil)")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("OnError never ran")
	}
}

func TestTheDefaultPlayerMatchesTheBuild(t *testing.T) {
	// The tests are run with -tags noaudio, where this is the stub; with
	// the real device it is skipped rather than opening one.
	if Available {
		t.Skip("built with audio: not opening a device from a test")
	}
	if err := New().Play("x.flac"); !errors.Is(err, ErrUnavailable) {
		t.Errorf("Play in a noaudio build: %v, want ErrUnavailable", err)
	}
}

// SetVolume reaches the stream that is playing, and a new stream (a track
// change or a seek) starts at the level already set.
func TestVolumeReachesTheStreamAndOutlivesIt(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "v.flac")
	writeFLAC(t, path, 44100, 16, [][]int32{make([]int32, 44100)})
	dev := &fakeDevice{voices: make(chan *fakeVoice, 4)}
	p := newWith(dev)
	if p.Volume() != 1 {
		t.Fatalf("a new player's level is %v, want 1", p.Volume())
	}
	p.SetVolume(0.4)
	if err := p.Play(path); err != nil {
		t.Fatal(err)
	}
	v := <-dev.voices
	p.SetVolume(1.7) // clamped
	v.mu.Lock()
	got := append([]float64(nil), v.vol...)
	v.mu.Unlock()
	if len(got) != 2 || got[0] != 0.4 || got[1] != 1 {
		t.Fatalf("the stream's levels %v, want [0.4 1]", got)
	}
	p.SetVolume(0.25)
	if err := p.Seek(0); err != nil {
		t.Fatal(err)
	}
	v2 := <-dev.voices
	v2.mu.Lock()
	first := append([]float64(nil), v2.vol...)
	v2.mu.Unlock()
	if len(first) == 0 || first[0] != 0.25 {
		t.Errorf("the stream after a seek started at %v, want 0.25", first)
	}
	p.Stop()
}

// The noaudio stub has no device and still keeps the level.
func TestTheStubKeepsTheVolume(t *testing.T) {
	p := New()
	p.SetVolume(0.3)
	if p.Volume() != 0.3 {
		t.Errorf("level %v, want 0.3", p.Volume())
	}
	p.SetVolume(-1)
	if p.Volume() != 0 {
		t.Errorf("level %v, want 0 (clamped)", p.Volume())
	}
}
