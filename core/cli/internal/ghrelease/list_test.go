package ghrelease

import (
	"context"
	"errors"
	"testing"
	"time"
)

func day(d int) time.Time { return time.Date(2026, 11, d, 12, 0, 0, 0, time.UTC) }

var fwAsset = []Asset{{Name: "core.ipod", Size: 1, URL: "x"}}

// A mixed list, the way the repo looks once the app ships apart: the newest
// release is the app's, a prerelease firmware is newer than the real one,
// and a draft is newer still. The firmware check picks v0.2.1; the app
// check picks app-v0.3.0.
func TestLatestFirmwareAndAppFromAMixedList(t *testing.T) {
	s := newAPIServer(t, map[string]Release{}, nil)
	s.list = []Release{
		{Tag: "app-v0.3.0", Published: day(5), URL: "https://example/app-v0.3.0"},
		{Tag: "v0.3.0", Published: day(6), Draft: true, Assets: fwAsset},
		{Tag: "v0.2.2-rc1", Published: day(4), Prerelease: true, Assets: fwAsset},
		{Tag: "v0.2.1", Published: day(1), Assets: fwAsset},
		{Tag: "v0.2.0", Published: day(0), Assets: fwAsset},
		{Tag: "app-v0.2.9", Published: day(3)},
		{Tag: "vscode-thing", Published: day(7), Assets: fwAsset},
	}
	c := s.client()
	fw, err := c.LatestFirmware(context.Background(), "owner/name")
	if err != nil || fw.Tag != "v0.2.1" {
		t.Errorf("LatestFirmware = %q, %v; want v0.2.1", fw.Tag, err)
	}
	app, err := c.LatestApp(context.Background(), "owner/name")
	if err != nil || app.Tag != "app-v0.3.0" || app.URL != "https://example/app-v0.3.0" {
		t.Errorf("LatestApp = %+v, %v", app, err)
	}
}

// A firmware tag without the image is not a firmware release (the old flow
// attached binaries before the image).
func TestLatestFirmwareNeedsTheImage(t *testing.T) {
	s := newAPIServer(t, map[string]Release{}, nil)
	s.list = []Release{
		{Tag: "v0.2.2", Published: day(5)},
		{Tag: "v0.2.1", Published: day(1), Assets: fwAsset},
	}
	fw, err := s.client().LatestFirmware(context.Background(), "owner/name")
	if err != nil || fw.Tag != "v0.2.1" {
		t.Errorf("LatestFirmware = %q, %v", fw.Tag, err)
	}
}

// Only app releases: the firmware check says not found, it does not return
// the app release.
func TestAppOnlyListDoesNotBreakTheFirmwareCheck(t *testing.T) {
	s := newAPIServer(t, map[string]Release{}, nil)
	s.list = []Release{{Tag: "app-v0.3.0", Published: day(5)}}
	if fw, err := s.client().LatestFirmware(context.Background(), "owner/name"); !errors.Is(err, ErrNotFound) {
		t.Errorf("LatestFirmware on an app-only list = %+v, %v", fw, err)
	}
}

func TestAppNewer(t *testing.T) {
	cases := []struct {
		cand, run string
		want      bool
	}{
		{"app-v0.3.1", "app-v0.3.0", true},
		{"app-v0.3.0", "app-v0.3.0", false},
		{"app-v0.3.0", "app-v0.3.1", false},
		{"app-v0.10.0", "app-v0.9.9", true}, // numeric, not lexical
		{"app-v1.0.0", "app-v0.99.99", true},
		{"app-v0.3.1", "app-v0.3.0-4-g1a2b3c4-dirty", true},
		{"app-v0.3.0", "app-v0.3.0-4-g1a2b3c4", false},
		{"app-v0.3.1", "(devel)", false},
		{"app-v0.3.1", "1a2b3c4", false},
		{"app-v0.3.1", "1a2b3c4-dirty", false},
		{"app-v0.3.1", "v0.2.1", false}, // a firmware-stamped build is not an app version
		{"v0.3.0", "app-v0.2.0", false},
	}
	for _, c := range cases {
		if got := AppNewer(c.cand, c.run); got != c.want {
			t.Errorf("AppNewer(%q, %q) = %v, want %v", c.cand, c.run, got, c.want)
		}
	}
}
