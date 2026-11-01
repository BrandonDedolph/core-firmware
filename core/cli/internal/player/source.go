package player

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/hajimehoshi/go-mp3"
	"github.com/mewkiz/flac"
)

// source is one open, decodable file: interleaved frames as int32 at the
// file's own rate, channel count and bit depth.
type source interface {
	// format is the decoded stream's rate, channels and bits per sample.
	format() (rate, channels, bits int)
	// total is the length in source frames, 0 when unknown.
	total() int64
	// next returns the next run of interleaved frames, io.EOF at the end.
	next() ([]int32, error)
	// seek moves to source frame f.
	seek(f int64) error
	close() error
}

// openSource picks the decoder by extension. The two formats the iPod
// plays are the two this preview plays.
func openSource(path string) (source, error) {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".flac":
		return openFLAC(path)
	case ".mp3":
		return openMP3(path)
	default:
		return nil, fmt.Errorf("%w: %s", ErrUnsupported, filepath.Ext(path))
	}
}

// checkPath is what Play can say at once, before the decoder goroutine
// starts: the file is there and is a format this package decodes.
func checkPath(path string) error {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".flac", ".mp3":
	default:
		return fmt.Errorf("%w: %q", ErrUnsupported, filepath.Ext(path))
	}
	fi, err := os.Stat(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("%w: %s", ErrMissing, filepath.Base(path))
		}
		return err
	}
	if fi.IsDir() {
		return fmt.Errorf("%s is a folder", filepath.Base(path))
	}
	return nil
}

// --- FLAC (github.com/mewkiz/flac) ---------------------------------------

type flacSource struct {
	f    *os.File
	s    *flac.Stream
	skip int64 // frames to drop after a seek landed on a frame boundary
}

func openFLAC(path string) (source, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	s, err := flac.NewSeek(f)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("not a FLAC stream this decoder reads: %w", err)
	}
	return &flacSource{f: f, s: s}, nil
}

func (x *flacSource) format() (int, int, int) {
	return int(x.s.Info.SampleRate), int(x.s.Info.NChannels), int(x.s.Info.BitsPerSample)
}

func (x *flacSource) total() int64 { return int64(x.s.Info.NSamples) }

func (x *flacSource) next() ([]int32, error) {
	for {
		fr, err := x.s.ParseNext()
		if err != nil {
			return nil, err
		}
		ch := len(fr.Subframes)
		n := int(fr.BlockSize)
		if ch == 0 || n == 0 {
			continue
		}
		start := 0
		if x.skip > 0 {
			if x.skip >= int64(n) {
				x.skip -= int64(n)
				continue
			}
			start, x.skip = int(x.skip), 0
		}
		out := make([]int32, 0, (n-start)*ch)
		for i := start; i < n; i++ {
			for c := 0; c < ch; c++ {
				out = append(out, fr.Subframes[c].Samples[i])
			}
		}
		return out, nil
	}
}

func (x *flacSource) seek(f int64) error {
	got, err := x.s.Seek(uint64(f))
	if err != nil {
		return err
	}
	x.skip = f - int64(got)
	if x.skip < 0 {
		x.skip = 0
	}
	return nil
}

func (x *flacSource) close() error { return x.f.Close() }

// --- MP3 (github.com/hajimehoshi/go-mp3) ---------------------------------

// mp3Source is go-mp3's decoder, which always yields 16-bit stereo.
type mp3Source struct {
	f   *os.File
	d   *mp3.Decoder
	buf []byte
}

func openMP3(path string) (source, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	d, err := mp3.NewDecoder(f)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("not an MP3 stream this decoder reads: %w", err)
	}
	return &mp3Source{f: f, d: d, buf: make([]byte, 4096*bytesPerFrame)}, nil
}

func (x *mp3Source) format() (int, int, int) { return x.d.SampleRate(), 2, 16 }

func (x *mp3Source) total() int64 {
	if l := x.d.Length(); l > 0 {
		return l / bytesPerFrame
	}
	return 0
}

func (x *mp3Source) next() ([]int32, error) {
	n, err := io.ReadFull(x.d, x.buf)
	if n == 0 {
		if err == io.ErrUnexpectedEOF || err == nil {
			err = io.EOF
		}
		return nil, err
	}
	n -= n % 2
	out := make([]int32, n/2)
	for i := range out {
		out[i] = int32(int16(uint16(x.buf[2*i]) | uint16(x.buf[2*i+1])<<8))
	}
	return out, nil
}

func (x *mp3Source) seek(f int64) error {
	_, err := x.d.Seek(f*bytesPerFrame, io.SeekStart)
	return err
}

func (x *mp3Source) close() error { return x.f.Close() }
