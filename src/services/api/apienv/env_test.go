package apienv_test

import (
	"testing"

	"github.com/spf13/viper"
	"github.com/stretchr/testify/assert"
	"pixielabs.ai/pixielabs/src/services/api/apienv"
)

func TestNew(t *testing.T) {
	viper.Set("session_key", "a-key")
	env, err := apienv.New(nil)
	assert.Nil(t, err)
	assert.NotNil(t, env)
	assert.NotNil(t, env.CookieStore())
}

func TestNew_MissingSessionKey(t *testing.T) {
	viper.Set("session_key", "")
	env, err := apienv.New(nil)
	assert.NotNil(t, err)
	assert.Nil(t, env)
}
