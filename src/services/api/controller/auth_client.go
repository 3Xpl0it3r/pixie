package controller

import (
	"github.com/spf13/pflag"
	"github.com/spf13/viper"
	"google.golang.org/grpc"
	authpb "pixielabs.ai/pixielabs/src/services/auth/proto"
	"pixielabs.ai/pixielabs/src/services/common"
)

func init() {
	pflag.String("auth_service", "auth-service.local:50100", "The auth service url (load balancer/list is ok)")
}

// NewAuthClient creates a new auth RPC client stub.
func NewAuthClient() (authpb.AuthServiceClient, error) {
	dialOpts, err := common.GetGRPCClientDialOpts()
	if err != nil {
		return nil, err
	}

	authChannel, err := grpc.Dial(viper.GetString("auth_service"), dialOpts...)
	if err != nil {
		return nil, err
	}

	return authpb.NewAuthServiceClient(authChannel), nil
}
