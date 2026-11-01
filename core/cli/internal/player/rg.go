package player

import (
	"path/filepath"
	"strings"

	coreflac "github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// ReplayGain, the firmware's way, ported line for line so the app and the
// iPod play a track at the same loudness:
//
//   - the tags are parsed as core/codecs/flac_meta.c does (parse_gain_q8:
//     dB to two decimals, in Q8, rounded; parse_peak_q16: linear peak to six
//     decimals, in Q16, rounded);
//   - core/player/player.c picks the TRACK gain and its peak, else the ALBUM
//     gain and its peak, and only for FLAC — an MP3 plays at unity on the
//     iPod, and so it does here;
//   - core/codecs/dr_flac/flac.c (flac_set_gain_db_q8) clamps the gain to
//     -30..+12 dB, turns it into a Q12 multiplier with its own exp2 table,
//     and caps it at full scale / peak — an unknown peak counting as full
//     scale, so it can attenuate but never boost — reporting the dB it
//     actually applied;
//   - apply_gain_s16 multiplies each int16 by the Q12 gain, rounds to
//     nearest and saturates.

// gainUnity is Q12 1.0.
const gainUnity = 4096

// Gain is what ReplayGain does to one track.
type Gain struct {
	// Have is false for a track with no gain tag (or an MP3): it plays
	// at unity.
	Have bool
	// AskedQ8 is the tagged gain and GotQ8 the gain applied, both in Q8 dB
	// (1/256 dB); Capped is GotQ8 < AskedQ8 — the peak held it down.
	AskedQ8, GotQ8 int
	Capped         bool
	// q12 is the multiplier.
	q12 int32
}

// DB is the applied gain in dB.
func (g Gain) DB() float64 { return float64(g.GotQ8) / 256 }

// parseGainQ8 is flac_meta.c's parse_gain_q8.
func parseGainQ8(s string) (int, bool) {
	j, neg := 0, false
	for j < len(s) && (s[j] == ' ' || s[j] == '\t') {
		j++
	}
	if j < len(s) && (s[j] == '+' || s[j] == '-') {
		neg = s[j] == '-'
		j++
	}
	whole, any := 0, false
	for j < len(s) && s[j] >= '0' && s[j] <= '9' {
		if whole < 1000 {
			whole = whole*10 + int(s[j]-'0')
		}
		any = true
		j++
	}
	frac := 0
	if j < len(s) && s[j] == '.' {
		j++
		for d := 0; d < 2; d++ {
			digit := 0
			if j < len(s) && s[j] >= '0' && s[j] <= '9' {
				digit = int(s[j] - '0')
				j++
			}
			frac = frac*10 + digit
			any = true
		}
	}
	if !any {
		return 0, false
	}
	q8 := ((whole*100+frac)*256 + 50) / 100
	if neg {
		q8 = -q8
	}
	return q8, true
}

// parsePeakQ16 is flac_meta.c's parse_peak_q16.
func parsePeakQ16(s string) (uint32, bool) {
	j := 0
	for j < len(s) && (s[j] == ' ' || s[j] == '\t') {
		j++
	}
	var whole uint64
	any := false
	for j < len(s) && s[j] >= '0' && s[j] <= '9' {
		if whole < 1000 {
			whole = whole*10 + uint64(s[j]-'0')
		}
		any = true
		j++
	}
	var frac uint64
	if j < len(s) && s[j] == '.' {
		j++
		for d := 0; d < 6; d++ {
			var digit uint64
			if j < len(s) && s[j] >= '0' && s[j] <= '9' {
				digit = uint64(s[j] - '0')
				j++
			}
			frac = frac*10 + digit
			any = true
		}
	}
	if !any {
		return 0, false
	}
	q16 := ((whole*1000000+frac)*65536 + 500000) / 1000000
	if q16 == 0 {
		return 0, false
	}
	return uint32(q16), true
}

// exp2Table is flac.c's EXP2_Q12.
var exp2Table = [17]int32{
	4096, 4277, 4467, 4664, 4871, 5087, 5312, 5547,
	5793, 6049, 6317, 6597, 6889, 7194, 7512, 7845, 8192,
}

// exp2Q12 is flac.c's exp2_q12: 2^(x/4096) in Q12.
func exp2Q12(x int32) int32 {
	oct := x >> 12
	fr := x - oct*4096
	i := fr >> 8
	t := fr & 0xFF
	lo := exp2Table[i]
	v := lo + (((exp2Table[i+1] - lo) * t) >> 8)
	v <<= 8
	if oct >= 0 {
		v <<= uint(oct)
	} else {
		v >>= uint(-oct)
	}
	return (v + 128) >> 8
}

const (
	gainMaxQ8 = 12 * 256
	gainMinQ8 = -30 * 256
)

func dbQ8ToGainQ12(db int) int32 { return exp2Q12((int32(db) * 680) >> 8) }

func gainQ12ToDbQ8(g int32) int {
	lo, hi := gainMinQ8, gainMaxQ8
	for lo < hi {
		mid := lo + (hi-lo+1)/2
		if dbQ8ToGainQ12(mid) <= g {
			lo = mid
		} else {
			hi = mid - 1
		}
	}
	return lo
}

// setGain is flac_set_gain_db_q8: the clamp, the Q12 gain, the peak cap.
func setGain(db int, peakQ16 uint32) (int32, int) {
	if db > gainMaxQ8 {
		db = gainMaxQ8
	} else if db < gainMinQ8 {
		db = gainMinQ8
	}
	g := dbQ8ToGainQ12(db)
	if peakQ16 == 0 {
		peakQ16 = 65536
	}
	if limit := int32((uint32(4096) << 16) / peakQ16); g > limit {
		g = limit
		db = gainQ12ToDbQ8(g)
	}
	return g, db
}

// gainFromTags is player.c's choice over a file's tags (keys lower-cased).
func gainFromTags(tags map[string]string) Gain {
	pick := func(gainKey, peakKey string) (Gain, bool) {
		asked, ok := parseGainQ8(tags[gainKey])
		if !ok {
			return Gain{}, false
		}
		peak, _ := parsePeakQ16(tags[peakKey])
		q12, got := setGain(asked, peak)
		return Gain{Have: true, AskedQ8: asked, GotQ8: got, Capped: got < asked, q12: q12}, true
	}
	if g, ok := pick("replaygain_track_gain", "replaygain_track_peak"); ok {
		return g
	}
	if g, ok := pick("replaygain_album_gain", "replaygain_album_peak"); ok {
		return g
	}
	return Gain{}
}

// trackGain reads one file's ReplayGain: FLAC only, as on the iPod.
func trackGain(path string) Gain {
	if !strings.EqualFold(filepath.Ext(path), ".flac") {
		return Gain{}
	}
	m, err := coreflac.ReadFile(path)
	if err != nil {
		return Gain{}
	}
	return gainFromTags(m.Tags)
}

// applyGain is apply_gain_s16, in place.
func applyGain(p []int16, g int32) {
	for i, s := range p {
		v := (int32(s)*g + 2048) >> 12
		if v > 32767 {
			v = 32767
		} else if v < -32768 {
			v = -32768
		}
		p[i] = int16(v)
	}
}
