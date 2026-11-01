package player

import (
	"encoding/binary"
	"math"
)

// The conversion from whatever a file holds to what the device plays:
// 44.1 kHz, two channels, signed 16-bit little-endian. Everything here is
// pure — buffers in, buffers out, state carried in a small struct — so the
// tests drive it with synthetic samples and no audio device.

// OutRate and OutChannels are the one format the process's audio context
// is opened in. A track at any other rate is resampled to it.
const (
	OutRate     = 44100
	OutChannels = 2
	// bytesPerFrame is one stereo frame of int16.
	bytesPerFrame = 4
)

// converter turns decoded frames (interleaved int32 at the source's bit
// depth and channel count) into interleaved stereo int16, with TPDF dither
// when the source is deeper than 16 bits.
type converter struct {
	channels, bits int
	rng            uint32
}

func newConverter(channels, bits int) *converter {
	return &converter{channels: channels, bits: bits, rng: 0x9E3779B9}
}

// next is xorshift32: a cheap, deterministic noise source for the dither.
func (c *converter) next() uint32 {
	x := c.rng
	x ^= x << 13
	x ^= x >> 17
	x ^= x << 5
	c.rng = x
	return x
}

// to16 brings one sample to 16 bits. Deeper samples are dithered with
// triangular noise one output LSB wide and rounded, then clamped; shallower
// ones are shifted up.
func (c *converter) to16(s int32) int16 {
	switch {
	case c.bits == 16 || c.bits <= 0:
		return clamp16(int64(s))
	case c.bits < 16:
		return clamp16(int64(s) << uint(16-c.bits))
	}
	shift := uint(c.bits - 16)
	mask := uint32(1)<<shift - 1
	// Two uniform draws in [0, 2^shift) summed and centred: a triangle on
	// ±(2^shift - 1), i.e. ±1 LSB of the 16-bit result.
	tpdf := int64(c.next()&mask) + int64(c.next()&mask) - int64(mask)
	v := int64(s) + tpdf + int64(1)<<(shift-1)
	return clamp16(v >> shift)
}

// stereo16 converts n interleaved source frames to stereo int16. Mono is
// written to both channels; more than two channels keep the first two
// (front left and right), which is what a preview listen needs.
func (c *converter) stereo16(in []int32) []int16 {
	ch := c.channels
	if ch < 1 {
		ch = 1
	}
	n := len(in) / ch
	out := make([]int16, 0, n*OutChannels)
	for i := 0; i < n; i++ {
		l := c.to16(in[i*ch])
		r := l
		if ch >= 2 {
			r = c.to16(in[i*ch+1])
		}
		out = append(out, l, r)
	}
	return out
}

func clamp16(v int64) int16 {
	switch {
	case v > 32767:
		return 32767
	case v < -32768:
		return -32768
	}
	return int16(v)
}

// resampler is a linear interpolator from one rate to OutRate over a
// stream that arrives in chunks. It keeps the last frame of the previous
// chunk and the fractional read position, so a stream fed in pieces comes
// out exactly as it would have in one piece.
type resampler struct {
	step float64 // source frames per output frame
	pos  float64 // read position, in frames of the current window
	prev [2]int16
	have bool
}

func newResampler(inRate int) *resampler {
	if inRate <= 0 || inRate == OutRate {
		return nil
	}
	return &resampler{step: float64(inRate) / OutRate}
}

// process resamples one chunk of interleaved stereo int16. A nil resampler
// passes the chunk through.
func (r *resampler) process(in []int16) []int16 {
	if r == nil {
		return in
	}
	n := len(in) / 2
	if n == 0 {
		return nil
	}
	// The window is the previous chunk's last frame (when there is one)
	// followed by this chunk.
	off := 0
	if r.have {
		off = 1
	}
	frames := n + off
	at := func(i int) (int16, int16) {
		if i < off {
			return r.prev[0], r.prev[1]
		}
		j := (i - off) * 2
		return in[j], in[j+1]
	}
	out := make([]int16, 0, int(float64(n)/r.step+2)*2)
	for r.pos+1 < float64(frames) {
		i := int(r.pos)
		t := r.pos - float64(i)
		l0, r0 := at(i)
		l1, r1 := at(i + 1)
		out = append(out, lerp(l0, l1, t), lerp(r0, r1, t))
		r.pos += r.step
	}
	r.pos -= float64(frames - 1)
	r.prev[0], r.prev[1] = at(frames - 1)
	r.have = true
	return out
}

// lerp is a + (b-a)·t, rounded to nearest.
func lerp(a, b int16, t float64) int16 {
	return clamp16(int64(math.Round(float64(a) + (float64(b)-float64(a))*t)))
}

// le16 is the wire format: interleaved int16, little-endian.
func le16(s []int16) []byte {
	b := make([]byte, len(s)*2)
	for i, v := range s {
		binary.LittleEndian.PutUint16(b[2*i:], uint16(v))
	}
	return b
}
