package controller_test

import (
	"context"
	"errors"
	"testing"

	"github.com/golang/mock/gomock"
	"github.com/stretchr/testify/assert"

	certmgrpb "pixielabs.ai/pixielabs/src/vizier/services/certmgr/certmgrpb"
	"pixielabs.ai/pixielabs/src/vizier/services/certmgr/controller"
	mock_controller "pixielabs.ai/pixielabs/src/vizier/services/certmgr/controller/mock"
)

func TestServer_UpdateCerts(t *testing.T) {
	ctrl := gomock.NewController(t)
	mockK8s := mock_controller.NewMockK8sAPI(ctrl)

	s := controller.NewServer(nil, mockK8s)

	req := &certmgrpb.UpdateCertsRequest{
		Key:  "abc",
		Cert: "def",
	}

	mockK8s.EXPECT().
		CreateTLSSecret("proxy-tls-certs", "abc", "def").
		Return(nil)

	mockK8s.EXPECT().
		GetPodNamesForService("vizier-proxy-service").
		Return([]string{"vizier-proxy-service-pod", "test"}, nil)

	mockK8s.EXPECT().
		DeletePod("vizier-proxy-service-pod").
		Return(nil)

	mockK8s.EXPECT().
		DeletePod("test").
		Return(nil)

	resp, err := s.UpdateCerts(context.Background(), req)
	assert.Nil(t, err)
	assert.NotNil(t, resp)
	assert.Equal(t, true, resp.OK)
}

func TestServer_UpdateCerts_SecretsFailed(t *testing.T) {
	ctrl := gomock.NewController(t)
	mockK8s := mock_controller.NewMockK8sAPI(ctrl)

	s := controller.NewServer(nil, mockK8s)

	req := &certmgrpb.UpdateCertsRequest{
		Key:  "abc",
		Cert: "def",
	}

	mockK8s.EXPECT().
		CreateTLSSecret("proxy-tls-certs", "abc", "def").
		Return(errors.New("Could not create secret"))

	resp, err := s.UpdateCerts(context.Background(), req)
	assert.Nil(t, resp)
	assert.NotNil(t, err)
}

func TestServer_UpdateCerts_NoPods(t *testing.T) {
	ctrl := gomock.NewController(t)
	mockK8s := mock_controller.NewMockK8sAPI(ctrl)

	s := controller.NewServer(nil, mockK8s)

	req := &certmgrpb.UpdateCertsRequest{
		Key:  "abc",
		Cert: "def",
	}

	mockK8s.EXPECT().
		CreateTLSSecret("proxy-tls-certs", "abc", "def").
		Return(nil)

	mockK8s.EXPECT().
		GetPodNamesForService("vizier-proxy-service").
		Return([]string{}, nil)

	resp, err := s.UpdateCerts(context.Background(), req)
	assert.Nil(t, resp)
	assert.NotNil(t, err)
}

func TestServer_UpdateCerts_FailedPodDeletion(t *testing.T) {
	ctrl := gomock.NewController(t)
	mockK8s := mock_controller.NewMockK8sAPI(ctrl)

	s := controller.NewServer(nil, mockK8s)

	req := &certmgrpb.UpdateCertsRequest{
		Key:  "abc",
		Cert: "def",
	}

	mockK8s.EXPECT().
		CreateTLSSecret("proxy-tls-certs", "abc", "def").
		Return(nil)

	mockK8s.EXPECT().
		GetPodNamesForService("vizier-proxy-service").
		Return([]string{"vizier-proxy-service-pod"}, nil)

	mockK8s.EXPECT().
		DeletePod("vizier-proxy-service-pod").
		Return(errors.New("Could not delete pod"))

	resp, err := s.UpdateCerts(context.Background(), req)
	assert.Nil(t, resp)
	assert.NotNil(t, err)
}
