package i18n

import (
	"testing"
	"time"
)

func TestFormatRelative(t *testing.T) {
	now := time.Date(2026, 3, 15, 12, 0, 0, 0, time.UTC)
	for _, c := range []struct {
		locale  string
		t       time.Time
		style   RelativeStyle
		numeric bool
		want    string
	}{
		{"en", now, RelativeLong, false, "now"},
		{"en", now.Add(500 * time.Millisecond), RelativeLong, true, "in 0 seconds"},
		{"en", now.Add(-30 * time.Second), RelativeLong, false, "30 seconds ago"},
		{"en", now.Add(-time.Minute), RelativeLong, false, "1 minute ago"},
		{"en", now.Add(-59 * time.Minute), RelativeShort, false, "59 min. ago"},
		{"en", now.Add(-3 * time.Hour), RelativeNarrow, false, "3h ago"},
		{"en", now.Add(-3 * time.Hour), RelativeLong, false, "3 hours ago"},
		{"en", now.Add(5 * time.Hour), RelativeLong, false, "in 5 hours"},
		// Yesterday at 23:00, at 01:00: hours, not a day.
		{"en", time.Date(2026, 3, 14, 23, 0, 0, 0, time.UTC), RelativeLong, false, "13 hours ago"},
		{"en", now.Add(-24 * time.Hour), RelativeLong, false, "yesterday"},
		{"en", now.Add(-24 * time.Hour), RelativeLong, true, "1 day ago"},
		{"en", now.Add(47 * time.Hour), RelativeLong, false, "tomorrow"},
		{"en", now.Add(-6 * 24 * time.Hour), RelativeLong, false, "6 days ago"},
		{"en", now.Add(-7 * 24 * time.Hour), RelativeLong, false, "last week"},
		{"en", now.Add(-20 * 24 * time.Hour), RelativeLong, false, "2 weeks ago"},
		{"en", time.Date(2026, 2, 15, 12, 0, 0, 0, time.UTC), RelativeLong, false, "last month"},
		{"en", time.Date(2025, 11, 1, 0, 0, 0, 0, time.UTC), RelativeLong, false, "4 months ago"},
		{"en", time.Date(2024, 3, 16, 0, 0, 0, 0, time.UTC), RelativeLong, false, "last year"},
		{"en", time.Date(2016, 3, 15, 12, 0, 0, 0, time.UTC), RelativeLong, false, "10 years ago"},
		{"en", time.Date(2028, 4, 1, 0, 0, 0, 0, time.UTC), RelativeShort, false, "in 2 yr."},
		{"en", now.Add(-1234 * 24 * time.Hour), RelativeLong, true, "3 years ago"},
		{"de", now.Add(-48 * time.Hour), RelativeLong, false, "vorgestern"},
		{"de", now.Add(-3 * time.Hour), RelativeLong, false, "vor 3 Stunden"},
		{"ru", now.Add(-time.Hour), RelativeLong, true, "1 час назад"},
		{"ru", now.Add(-3 * time.Hour), RelativeLong, false, "3 часа назад"},
		{"ru", now.Add(-5 * time.Hour), RelativeLong, false, "5 часов назад"},
		{"ru", now.Add(-21 * time.Minute), RelativeLong, false, "21 минуту назад"},
		{"ar", now.Add(-2 * time.Hour), RelativeLong, false, "قبل ساعتين"},
		{"ja", now.Add(-24 * time.Hour), RelativeLong, false, "昨日"},
		{"ja", now.Add(3 * time.Hour), RelativeLong, false, "3 時間後"},
	} {
		if got := FormatRelative(c.locale, c.t, now, c.style, c.numeric); got != c.want {
			t.Errorf("%s %v (style %d, numeric %v): %q, want %q", c.locale, c.t.Sub(now), c.style, c.numeric, got, c.want)
		}
	}
}
