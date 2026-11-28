package main

import (
	"encoding/xml"
	"os"
	"path/filepath"
	"time"
)

// JUnit's XML, which CI services read a run's tests from (GitHub's test
// reporters, GitLab's, Jenkins', CircleCI's, Buildkite's): asuka test's
// --junit and asuka test --gui's.

type junitSuites struct {
	XMLName  xml.Name     `xml:"testsuites"`
	Name     string       `xml:"name,attr,omitempty"`
	Tests    int          `xml:"tests,attr"`
	Failures int          `xml:"failures,attr"`
	Time     float64      `xml:"time,attr"`
	Suites   []junitSuite `xml:"testsuite"`
}

type junitSuite struct {
	Name      string      `xml:"name,attr"`
	Tests     int         `xml:"tests,attr"`
	Failures  int         `xml:"failures,attr"`
	Time      float64     `xml:"time,attr"`
	Timestamp string      `xml:"timestamp,attr,omitempty"`
	Cases     []junitCase `xml:"testcase"`
}

type junitCase struct {
	Name      string        `xml:"name,attr"`
	Classname string        `xml:"classname,attr"`
	File      string        `xml:"file,attr,omitempty"`
	Line      int           `xml:"line,attr,omitempty"`
	Time      float64       `xml:"time,attr"`
	Failure   *junitFailure `xml:"failure,omitempty"`
}

type junitFailure struct {
	Message string `xml:"message,attr"`
	Text    string `xml:",chardata"`
}

// add puts a case in the suite, counting it.
func (s *junitSuite) add(c junitCase) {
	s.Cases = append(s.Cases, c)
	s.Tests++
	if c.Failure != nil {
		s.Failures++
	}
}

func newJUnitSuite(name string, start time.Time) junitSuite {
	return junitSuite{Name: name, Timestamp: start.UTC().Format("2006-01-02T15:04:05")}
}

// writeJUnit writes the suites to path as JUnit's XML.
func writeJUnit(path, name string, suites []junitSuite) error {
	all := junitSuites{Name: name, Suites: suites}
	for _, s := range suites {
		all.Tests += s.Tests
		all.Failures += s.Failures
		all.Time += s.Time
	}
	b, err := xml.MarshalIndent(all, "", "  ")
	if err != nil {
		return err
	}
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	return os.WriteFile(path, append([]byte(xml.Header), append(b, '\n')...), 0o644)
}
