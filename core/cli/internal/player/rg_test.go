package player

import (
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/mewkiz/flac"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/flac/meta"
)

// The gain chain against the firmware: these rows are what
// core/codecs/flac_meta.c's parsers and core/codecs/dr_flac/flac.c's
// flac_set_gain_db_q8 print when compiled and run on the same tag strings
// (gain Q8, peak Q16, applied Q8, Q12 multiplier).
func TestReplayGainMatchesTheFirmware(t *testing.T) {
	rows := []struct {
		gain, peak string
		q8         int
		pk         uint32
		got        int
		q12        int32
	}{
		{"-8.30 dB", "", -2125, 0, -2125, 1576},
		{"+6.00 dB", "1.000000", 1536, 65536, 0, 4096},
		{"-0.5", "0.5", -128, 32768, -128, 3868},
		{"+12.5 dB", "0.988525", 3200, 64784, 25, 4143},
		{"-35", "0.25", -8960, 16384, -7680, 130},
		{"3.456", "", 883, 0, 0, 4096},
		{"+0.01", "", 3, 0, 0, 4096},
	}
	for _, r := range rows {
		q8, ok := parseGainQ8(r.gain)
		if !ok || q8 != r.q8 {
			t.Errorf("gain %q: %d (%v), firmware %d", r.gain, q8, ok, r.q8)
		}
		var pk uint32
		if r.peak != "" {
			pk, _ = parsePeakQ16(r.peak)
		}
		if pk != r.pk {
			t.Errorf("peak %q: %d, firmware %d", r.peak, pk, r.pk)
		}
		q12, got := setGain(q8, pk)
		if got != r.got || q12 != r.q12 {
			t.Errorf("%q peak %q: applied %d (Q12 %d), firmware %d (Q12 %d)", r.gain, r.peak, got, q12, r.got, r.q12)
		}
	}
	// Track gain wins; album gain is the fallback; no tag is unity.
	g := gainFromTags(map[string]string{"replaygain_track_gain": "-3.00 dB",
		"replaygain_album_gain": "-9.00 dB"})
	if !g.Have || g.AskedQ8 != -768 {
		t.Errorf("track over album: %+v", g)
	}
	g = gainFromTags(map[string]string{"replaygain_album_gain": "-9.00 dB", "replaygain_album_peak": "0.9"})
	if !g.Have || g.AskedQ8 != -2304 {
		t.Errorf("album fallback: %+v", g)
	}
	if g := gainFromTags(map[string]string{"title": "x"}); g.Have {
		t.Errorf("no tag: %+v", g)
	}
}

// writeTaggedFLAC is writeFLAC with a Vorbis comment.
func writeTaggedFLAC(t *testing.T, path string, rate, bits int, chans [][]int32, tags [][2]string) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	n := len(chans[0])
	info := &meta.StreamInfo{BlockSizeMin: 4096, BlockSizeMax: 4096, SampleRate: uint32(rate),
		NChannels: uint8(len(chans)), BitsPerSample: uint8(bits), NSamples: uint64(n)}
	var blocks []*meta.Block
	if len(tags) > 0 {
		// The encoder writes the header it is given, so the body's length
		// is ours to count: vendor, count, then each "KEY=value".
		n := int64(4 + len("test") + 4)
		for _, kv := range tags {
			n += int64(4 + len(kv[0]) + 1 + len(kv[1]))
		}
		blocks = append(blocks, &meta.Block{Header: meta.Header{Type: meta.TypeVorbisComment, Length: n},
			Body: &meta.VorbisComment{Vendor: "test", Tags: tags}})
	}
	enc, err := flac.NewEncoder(f, info, blocks...)
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

// sine is one second of a 1 kHz tone at dBFS, at the file's depth.
func sine(rate, bits int, dbfs float64) []int32 {
	full := float64(int64(1)<<(bits-1) - 1)
	amp := full * math.Pow(10, dbfs/20)
	out := make([]int32, rate)
	for i := range out {
		out[i] = int32(math.Round(amp * math.Sin(2*math.Pi*1000*float64(i)/float64(rate))))
	}
	return out
}

// played runs one file through the whole player — decode, 24→16, mono
// doubled, resample, ReplayGain — into a fake device, and returns the
// peaks of what reached it, in dBFS, left and right.
func played(t *testing.T, path string, rg bool) (float64, float64) {
	t.Helper()
	dev := &fakeDevice{voices: make(chan *fakeVoice, 4)}
	p := newWith(dev)
	p.SetReplayGain(rg)
	done := make(chan struct{})
	p.OnEnd(func() { close(done) })
	p.OnError(func(err error) { t.Errorf("%s: %v", filepath.Base(path), err); close(done) })
	if err := p.Play(path); err != nil {
		t.Fatal(err)
	}
	v := <-dev.voices
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("the track did not end")
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	var pk [2]int
	for i := 0; i+3 < len(v.got); i += 4 {
		for c := 0; c < 2; c++ {
			s := int(int16(uint16(v.got[i+2*c]) | uint16(v.got[i+2*c+1])<<8))
			if s < 0 {
				s = -s
			}
			if s > pk[c] {
				pk[c] = s
			}
		}
	}
	db := func(n int) float64 { return 20 * math.Log10(float64(n)/32767) }
	return db(pk[0]), db(pk[1])
}

// The gain audit: a -20 dBFS tone comes out of the player at -20 dBFS,
// whatever the file's rate, depth and channel count — nothing on the way
// adds level, a mono file is not summed into +6 dB — and with ReplayGain
// on, a -6 dB tag takes 6 dB off and a +6 dB tag with a full-scale peak is
// held at unity by the cap.
func TestGainAudit(t *testing.T) {
	dir := t.TempDir()
	cases := []struct {
		name       string
		rate, bits int
		mono       bool
	}{
		{"16-bit stereo 44.1 kHz", 44100, 16, false},
		{"16-bit stereo 48 kHz (resampled)", 48000, 16, false},
		{"24-bit stereo 44.1 kHz (dithered to 16)", 44100, 24, false},
		{"16-bit mono 44.1 kHz (doubled)", 44100, 16, true},
	}
	for i, c := range cases {
		tone := sine(c.rate, c.bits, -20)
		chans := [][]int32{tone, tone}
		if c.mono {
			chans = chans[:1]
		}
		path := filepath.Join(dir, "tone"+itoa(i)+".flac")
		writeTaggedFLAC(t, path, c.rate, c.bits, chans, nil)
		l, r := played(t, path, false)
		t.Logf("%s: left %.2f dBFS, right %.2f dBFS", c.name, l, r)
		if math.Abs(l+20) > 0.2 || math.Abs(r+20) > 0.2 {
			t.Errorf("%s: %.2f / %.2f dBFS, want -20 ± 0.2", c.name, l, r)
		}
	}

	tone := sine(44100, 16, -20)
	minus6 := filepath.Join(dir, "minus6.flac")
	writeTaggedFLAC(t, minus6, 44100, 16, [][]int32{tone, tone},
		[][2]string{{"REPLAYGAIN_TRACK_GAIN", "-6.00 dB"}, {"REPLAYGAIN_TRACK_PEAK", "0.100000"}})
	l, _ := played(t, minus6, true)
	t.Logf("-6 dB tag, ReplayGain on: %.2f dBFS", l)
	if math.Abs(l+26) > 0.2 {
		t.Errorf("a -6 dB tag: %.2f dBFS, want -26", l)
	}
	if l, _ := played(t, minus6, false); math.Abs(l+20) > 0.2 {
		t.Errorf("ReplayGain off still changed the level: %.2f dBFS", l)
	}
	plus6 := filepath.Join(dir, "plus6.flac")
	writeTaggedFLAC(t, plus6, 44100, 16, [][]int32{tone, tone},
		[][2]string{{"REPLAYGAIN_TRACK_GAIN", "+6.00 dB"}, {"REPLAYGAIN_TRACK_PEAK", "1.000000"}})
	l, _ = played(t, plus6, true)
	t.Logf("+6 dB tag, full-scale peak: %.2f dBFS (capped at unity)", l)
	if math.Abs(l+20) > 0.2 {
		t.Errorf("a capped +6 dB tag: %.2f dBFS, want -20", l)
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	s := ""
	for ; n > 0; n /= 10 {
		s = string(rune('0'+n%10)) + s
	}
	return s
}
