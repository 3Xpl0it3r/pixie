package utils_test

import (
	"fmt"
	"testing"

	"github.com/alecthomas/assert"
	"pixielabs.ai/pixielabs/src/utils/pixie_cli/pkg/utils"
)

func TestVersionCompatible(t *testing.T) {
	tests := []struct {
		minVersion  string
		testVersion string
		ok          bool
		expectErr   bool
	}{
		{
			minVersion:  "4.14.0",
			testVersion: "4.14.165-133.209.amzn2.x86_64",
			ok:          true,
		},
		{
			minVersion:  "4.15.0",
			testVersion: "4.14.165-133.209",
			ok:          false,
		},
		{
			minVersion:  "4.15.0",
			testVersion: "4.15.0",
			ok:          true,
		},
		{
			minVersion:  "4.15.0",
			testVersion: "a4",
			expectErr:   true,
		},
	}

	for _, test := range tests {
		name := fmt.Sprintf("Check %s < %s", test.minVersion, test.testVersion)
		t.Run(name, func(t *testing.T) {
			ok, err := utils.VersionCompatible(test.testVersion, test.minVersion)
			if test.expectErr {
				assert.NotNil(t, err)
				return
			}
			assert.Nil(t, err)
			assert.Equal(t, test.ok, ok)
		})
	}
}
