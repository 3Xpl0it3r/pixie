package certmgrenv

import "pixielabs.ai/pixielabs/src/shared/services/env"

// CertMgrEnv is the interface for the certmgr service environment.
type CertMgrEnv interface {
	env.Env
}

// Impl is an implementation of the CertMgrEnv interface
type Impl struct {
	*env.BaseEnv
}

// New creates a new certmgr env.
func New() *Impl {
	return &Impl{env.New()}
}
