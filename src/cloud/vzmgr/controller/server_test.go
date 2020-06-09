package controller_test

import (
	"context"
	"errors"
	"sort"
	"testing"
	"time"

	"github.com/dgrijalva/jwt-go"
	"github.com/gogo/protobuf/proto"
	"github.com/gogo/protobuf/types"
	bindata "github.com/golang-migrate/migrate/source/go_bindata"
	"github.com/golang/mock/gomock"
	"github.com/jmoiron/sqlx"
	"github.com/nats-io/nats.go"
	uuid "github.com/satori/go.uuid"
	"github.com/spf13/viper"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"pixielabs.ai/pixielabs/src/cloud/artifact_tracker/artifacttrackerpb"
	mock_artifacttrackerpb "pixielabs.ai/pixielabs/src/cloud/artifact_tracker/artifacttrackerpb/mock"
	dnsmgrpb "pixielabs.ai/pixielabs/src/cloud/dnsmgr/dnsmgrpb"
	mock_dnsmgrpb "pixielabs.ai/pixielabs/src/cloud/dnsmgr/dnsmgrpb/mock"
	messagespb "pixielabs.ai/pixielabs/src/cloud/shared/messagespb"
	"pixielabs.ai/pixielabs/src/cloud/shared/vzshard"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/controller"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/schema"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/vzerrors"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/vzmgrpb"
	"pixielabs.ai/pixielabs/src/shared/artifacts/versionspb"
	"pixielabs.ai/pixielabs/src/shared/cvmsgspb"
	"pixielabs.ai/pixielabs/src/shared/services/authcontext"
	"pixielabs.ai/pixielabs/src/shared/services/pgtest"
	jwtutils "pixielabs.ai/pixielabs/src/shared/services/utils"
	"pixielabs.ai/pixielabs/src/utils"
	"pixielabs.ai/pixielabs/src/utils/testingutils"
)

type vizierStatus cvmsgspb.VizierStatus

func setupTestDB(t *testing.T) (*sqlx.DB, func()) {
	s := bindata.Resource(schema.AssetNames(), func(name string) (bytes []byte, e error) {
		return schema.Asset(name)
	})
	db, teardown := pgtest.SetupTestDB(t, s)

	return db, func() {
		teardown()
	}
}

var (
	testAuthOrgID                   = "223e4567-e89b-12d3-a456-426655440000"
	testNonAuthOrgID                = "223e4567-e89b-12d3-a456-426655440001"
	testProjectName                 = "foo"
	testDisconnectedClusterEmptyUID = "123e4567-e89b-12d3-a456-426655440003"
	testExistingCluster             = "823e4567-e89b-12d3-a456-426655440008"
	testExistingClusterActive       = "923e4567-e89b-12d3-a456-426655440008"
)

func loadTestData(t *testing.T, db *sqlx.DB) {
	insertVizierClusterQuery := `INSERT INTO vizier_cluster(org_id, id, project_name, cluster_uid, cluster_version, cluster_name) VALUES ($1, $2, $3, $4, $5, $6)`
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, "123e4567-e89b-12d3-a456-426655440000", testProjectName, "", "", "unknown_cluster")
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, "123e4567-e89b-12d3-a456-426655440001", testProjectName, "cUID", "cVers", "healthy_cluster")
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, "123e4567-e89b-12d3-a456-426655440002", testProjectName, "", "", "unhealthy_cluster")
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, testDisconnectedClusterEmptyUID, testProjectName, "", "", "disconnected_cluster")
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, testExistingCluster, testProjectName, "existing_cluster", "", "test-cluster")
	db.MustExec(insertVizierClusterQuery, testAuthOrgID, testExistingClusterActive, testProjectName, "my_other_cluster", "", "existing_cluster")
	db.MustExec(insertVizierClusterQuery, testNonAuthOrgID, "223e4567-e89b-12d3-a456-426655440003", testProjectName, "", "", "non_auth_1")
	db.MustExec(insertVizierClusterQuery, testNonAuthOrgID, "323e4567-e89b-12d3-a456-426655440003", testProjectName, "", "", "non_auth_2")

	insertVizierClusterInfoQuery := `INSERT INTO vizier_cluster_info(vizier_cluster_id, status, address, jwt_signing_key, last_heartbeat, passthrough_enabled, vizier_version) VALUES($1, $2, $3, $4, $5, $6, $7)`
	db.MustExec(insertVizierClusterInfoQuery, "123e4567-e89b-12d3-a456-426655440000", "UNKNOWN", "addr0", "key0", "2011-05-16 15:36:38", true, "")
	db.MustExec(insertVizierClusterInfoQuery, "123e4567-e89b-12d3-a456-426655440001", "HEALTHY", "addr1", "\\xc30d04070302c5374a5098262b6d7bd23f01822f741dbebaa680b922b55fd16eb985aeb09505f8fc4a36f0e11ebb8e18f01f684146c761e2234a81e50c21bca2907ea37736f2d9a5834997f4dd9e288c", "2011-05-17 15:36:38", false, "vzVers")
	db.MustExec(insertVizierClusterInfoQuery, "123e4567-e89b-12d3-a456-426655440002", "UNHEALTHY", "addr2", "key2", "2011-05-18 15:36:38", true, "")
	db.MustExec(insertVizierClusterInfoQuery, testDisconnectedClusterEmptyUID, "DISCONNECTED", "addr3", "key3", "2011-05-19 15:36:38", false, "")
	db.MustExec(insertVizierClusterInfoQuery, testExistingCluster, "DISCONNECTED", "addr3", "key3", "2011-05-19 15:36:38", false, "")
	db.MustExec(insertVizierClusterInfoQuery, testExistingClusterActive, "UNHEALTHY", "addr3", "key3", "2011-05-19 15:36:38", false, "")
	db.MustExec(insertVizierClusterInfoQuery, "223e4567-e89b-12d3-a456-426655440003", "HEALTHY", "addr3", "key3", "2011-05-19 15:36:38", true, "")
	db.MustExec(insertVizierClusterInfoQuery, "323e4567-e89b-12d3-a456-426655440003", "HEALTHY", "addr3", "key3", "2011-05-19 15:36:38", false, "")

	db.MustExec(`UPDATE vizier_cluster SET cluster_name=NULL WHERE id=$1`, testDisconnectedClusterEmptyUID)
	insertVizierIndexQuery := `INSERT INTO vizier_index_state(cluster_id, resource_version) VALUES($1, $2)`
	db.MustExec(insertVizierIndexQuery, "123e4567-e89b-12d3-a456-426655440001", "1234")
}

func CreateTestContext() context.Context {
	sCtx := authcontext.New()
	sCtx.Claims = jwtutils.GenerateJWTForUser("abcdef", testAuthOrgID, "test@test.com", time.Now())
	return authcontext.NewContext(context.Background(), sCtx)
}

func TestServer_GetViziersByOrg(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)

	t.Run("valid", func(t *testing.T) {
		// Fetch the test data that was inserted earlier.
		resp, err := s.GetViziersByOrg(CreateTestContext(), utils.ProtoFromUUIDStrOrNil(testAuthOrgID))
		require.Nil(t, err)
		require.NotNil(t, resp)

		assert.Equal(t, len(resp.VizierIDs), 6)

		// Convert to UUID string array.
		var ids []string
		for _, val := range resp.VizierIDs {
			ids = append(ids, utils.UUIDFromProtoOrNil(val).String())
		}
		sort.Strings(ids)
		expected := []string{
			"123e4567-e89b-12d3-a456-426655440000",
			"123e4567-e89b-12d3-a456-426655440001",
			"123e4567-e89b-12d3-a456-426655440002",
			"123e4567-e89b-12d3-a456-426655440003",
			"823e4567-e89b-12d3-a456-426655440008",
			"923e4567-e89b-12d3-a456-426655440008",
		}
		sort.Strings(expected)
		assert.Equal(t, ids, expected)
	})

	t.Run("No such org id", func(t *testing.T) {
		resp, err := s.GetViziersByOrg(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("323e4567-e89b-12d3-a456-426655440000"))
		require.NotNil(t, err)
		assert.Nil(t, resp)
		assert.Equal(t, status.Code(err), codes.PermissionDenied)
	})

	t.Run("bad input org id", func(t *testing.T) {
		resp, err := s.GetViziersByOrg(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("3e4567-e89b-12d3-a456-426655440000"))
		require.NotNil(t, err)
		assert.Nil(t, resp)
		assert.Equal(t, status.Code(err), codes.InvalidArgument)
	})

	t.Run("missing input org id", func(t *testing.T) {
		resp, err := s.GetViziersByOrg(CreateTestContext(), nil)
		require.NotNil(t, err)
		assert.Nil(t, resp)
		assert.Equal(t, status.Code(err), codes.InvalidArgument)
	})

	t.Run("mismatched input org id", func(t *testing.T) {
		resp, err := s.GetViziersByOrg(CreateTestContext(), utils.ProtoFromUUIDStrOrNil(testNonAuthOrgID))
		require.NotNil(t, err)
		assert.Nil(t, resp)
		assert.Equal(t, status.Code(err), codes.PermissionDenied)
	})
}

func TestServer_GetVizierInfo(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)
	resp, err := s.GetVizierInfo(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"))
	require.Nil(t, err)
	require.NotNil(t, resp)

	assert.Equal(t, resp.VizierID, utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"))
	assert.Equal(t, resp.Status, cvmsgspb.VZ_ST_HEALTHY)
	assert.Greater(t, resp.LastHeartbeatNs, int64(0))
	assert.Equal(t, resp.Config.PassthroughEnabled, false)
	assert.Equal(t, "vzVers", resp.VizierVersion)
	assert.Equal(t, "cVers", resp.ClusterVersion)
	assert.Equal(t, "healthy_cluster", resp.ClusterName)
	assert.Equal(t, "cUID", resp.ClusterUID)
}

func TestServer_UpdateVizierConfig(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)
	resp, err := s.UpdateVizierConfig(CreateTestContext(), &cvmsgspb.UpdateVizierConfigRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		ConfigUpdate: &cvmsgspb.VizierConfigUpdate{
			PassthroughEnabled: &types.BoolValue{Value: true},
		},
	})
	require.Nil(t, err)
	require.NotNil(t, resp)

	// Check that the value was actually updated.
	infoResp, err := s.GetVizierInfo(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"))
	require.Nil(t, err)
	require.NotNil(t, infoResp)
	assert.Equal(t, infoResp.Config.PassthroughEnabled, true)
}

func TestServer_UpdateVizierConfig_WrongOrg(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)
	resp, err := s.UpdateVizierConfig(CreateTestContext(), &cvmsgspb.UpdateVizierConfigRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil("223e4567-e89b-12d3-a456-426655440003"),
		ConfigUpdate: &cvmsgspb.VizierConfigUpdate{
			PassthroughEnabled: &types.BoolValue{Value: true},
		},
	})
	require.Nil(t, resp)
	require.NotNil(t, err)
	assert.Equal(t, status.Code(err), codes.NotFound)
}

func TestServer_UpdateVizierConfig_NoUpdates(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)
	resp, err := s.UpdateVizierConfig(CreateTestContext(), &cvmsgspb.UpdateVizierConfigRequest{
		VizierID:     utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		ConfigUpdate: &cvmsgspb.VizierConfigUpdate{},
	})
	require.Nil(t, err)
	require.NotNil(t, resp)

	// Check that the value was not updated.
	infoResp, err := s.GetVizierInfo(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"))
	require.Nil(t, err)
	require.NotNil(t, infoResp)
	assert.Equal(t, infoResp.Config.PassthroughEnabled, false)
}

func TestServer_GetVizierConnectionInfo(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nil)
	resp, err := s.GetVizierConnectionInfo(CreateTestContext(), utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"))
	require.Nil(t, err)
	require.NotNil(t, resp)

	assert.Equal(t, resp.IPAddress, "https://addr1")
	assert.NotNil(t, resp.Token)
	claims := jwt.MapClaims{}
	_, err = jwt.ParseWithClaims(resp.Token, claims, func(token *jwt.Token) (interface{}, error) {
		return []byte("key0"), nil
	})
	assert.Nil(t, err)
	assert.Equal(t, "cluster", claims["Scopes"].(string))

	// TODO(zasgar): write more tests here.
}

func TestServer_VizierConnectedUnhealthy(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()
	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)
	req := &cvmsgspb.RegisterVizierRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		JwtKey:   "the-token",
		ClusterInfo: &cvmsgspb.VizierClusterInfo{
			ClusterUID: "cUID",
		},
	}

	resp, err := s.VizierConnected(context.Background(), req)
	require.Nil(t, err)
	require.NotNil(t, resp)

	assert.Equal(t, resp.Status, cvmsgspb.ST_OK)

	// Check to make sure DB insert for JWT signing key is correct.
	clusterQuery := `SELECT PGP_SYM_DECRYPT(jwt_signing_key::bytea, 'test') as jwt_signing_key, status from vizier_cluster_info WHERE vizier_cluster_id=$1`

	var clusterInfo struct {
		JWTSigningKey string `db:"jwt_signing_key"`
		Status        string `db:"status"`
	}
	clusterID, err := uuid.FromString("123e4567-e89b-12d3-a456-426655440001")
	assert.Nil(t, err)
	err = db.Get(&clusterInfo, clusterQuery, clusterID)
	assert.Nil(t, err)
	assert.Equal(t, "the-token", clusterInfo.JWTSigningKey[controller.SaltLength:])

	assert.Equal(t, "UNHEALTHY", clusterInfo.Status)
	// TODO(zasgar): write more tests here.
}

func TestServer_VizierConnectedHealthy(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()
	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	subCh := make(chan *nats.Msg, 1)
	natsSub, err := nc.ChanSubscribe("VizierConnected", subCh)
	defer natsSub.Unsubscribe()

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)
	req := &cvmsgspb.RegisterVizierRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		JwtKey:   "the-token",
		Address:  "127.0.0.1",
		ClusterInfo: &cvmsgspb.VizierClusterInfo{
			ClusterUID:     "cUID",
			ClusterVersion: "1234",
			VizierVersion:  "some version",
		},
	}

	resp, err := s.VizierConnected(context.Background(), req)
	require.Nil(t, err)
	require.NotNil(t, resp)

	assert.Equal(t, resp.Status, cvmsgspb.ST_OK)

	// Check to make sure DB insert for JWT signing key is correct.
	clusterQuery := `SELECT PGP_SYM_DECRYPT(jwt_signing_key::bytea, 'test') as jwt_signing_key, status, vizier_version from vizier_cluster_info WHERE vizier_cluster_id=$1`

	var clusterInfo struct {
		JWTSigningKey string `db:"jwt_signing_key"`
		Status        string `db:"status"`
		VizierVersion string `db:"vizier_version"`
	}
	clusterID, err := uuid.FromString("123e4567-e89b-12d3-a456-426655440001")
	assert.Nil(t, err)
	err = db.Get(&clusterInfo, clusterQuery, clusterID)
	assert.Nil(t, err)
	assert.Equal(t, "CONNECTED", clusterInfo.Status)
	assert.Equal(t, "some version", clusterInfo.VizierVersion)

	select {
	case msg := <-subCh:
		req := &messagespb.VizierConnected{}
		err := proto.Unmarshal(msg.Data, req)
		assert.Nil(t, err)
		assert.Equal(t, "1234", req.ResourceVersion)
		assert.Equal(t, "cUID", req.K8sUID)
		assert.Equal(t, "223e4567-e89b-12d3-a456-426655440000", utils.UUIDFromProtoOrNil(req.OrgID).String())
		assert.Equal(t, "123e4567-e89b-12d3-a456-426655440001", utils.UUIDFromProtoOrNil(req.VizierID).String())
		return
	case <-time.After(1 * time.Second):
		t.Fatal("Timed out")
		return
	}
	// TODO(zasgar): write more tests here.
}

func TestServer_HandleVizierHeartbeat(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()

	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)

	tests := []struct {
		name                       string
		expectGetDNSAddressCalled  bool
		dnsAddressResponse         *dnsmgrpb.GetDNSAddressResponse
		dnsAddressError            error
		vizierID                   string
		hbAddress                  string
		hbPort                     int
		updatedClusterStatus       string
		expectedClusterAddress     string
		status                     cvmsgspb.VizierStatus
		bootstrap                  bool
		bootstrapVersion           string
		expectDeploy               bool
		expectedFetchVizierVersion bool
	}{
		{
			name:                      "valid vizier",
			expectGetDNSAddressCalled: true,
			dnsAddressResponse: &dnsmgrpb.GetDNSAddressResponse{
				DNSAddress: "abc.clusters.dev.withpixie.dev",
			},
			dnsAddressError:        nil,
			vizierID:               "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:              "127.0.0.1",
			hbPort:                 123,
			updatedClusterStatus:   "HEALTHY",
			expectedClusterAddress: "abc.clusters.dev.withpixie.dev:123",
		},
		{
			name:                      "valid vizier dns failed",
			expectGetDNSAddressCalled: true,
			dnsAddressResponse:        nil,
			dnsAddressError:           errors.New("Could not get DNS address"),
			vizierID:                  "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:                 "127.0.0.1",
			hbPort:                    123,
			updatedClusterStatus:      "HEALTHY",
			expectedClusterAddress:    "127.0.0.1:123",
		},
		{
			name:                      "valid vizier no address",
			expectGetDNSAddressCalled: false,
			vizierID:                  "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:                 "",
			hbPort:                    0,
			updatedClusterStatus:      "UNHEALTHY",
			expectedClusterAddress:    "",
		},
		{
			name:                      "unknown vizier",
			expectGetDNSAddressCalled: false,
			vizierID:                  "223e4567-e89b-12d3-a456-426655440001",
			hbAddress:                 "",
			updatedClusterStatus:      "",
			expectedClusterAddress:    "",
			status:                    cvmsgspb.VZ_ST_UPDATING,
		},
		{
			name:                      "bootstrap vizier",
			expectGetDNSAddressCalled: true,
			dnsAddressResponse: &dnsmgrpb.GetDNSAddressResponse{
				DNSAddress: "abc.clusters.dev.withpixie.dev",
			},
			dnsAddressError:            nil,
			vizierID:                   "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:                  "127.0.0.1",
			hbPort:                     123,
			updatedClusterStatus:       "UPDATING",
			expectedClusterAddress:     "abc.clusters.dev.withpixie.dev:123",
			status:                     cvmsgspb.VZ_ST_UPDATING,
			bootstrap:                  true,
			bootstrapVersion:           "",
			expectedFetchVizierVersion: true,
			expectDeploy:               true,
		},
		{
			name:                      "bootstrap updating vizier",
			expectGetDNSAddressCalled: true,
			dnsAddressResponse: &dnsmgrpb.GetDNSAddressResponse{
				DNSAddress: "abc.clusters.dev.withpixie.dev",
			},
			dnsAddressError:            nil,
			vizierID:                   "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:                  "127.0.0.1",
			hbPort:                     123,
			updatedClusterStatus:       "UPDATING",
			expectedClusterAddress:     "abc.clusters.dev.withpixie.dev:123",
			status:                     cvmsgspb.VZ_ST_UPDATING,
			bootstrap:                  true,
			bootstrapVersion:           "",
			expectedFetchVizierVersion: false,
			expectDeploy:               false,
		},
		{
			name:                      "updating vizier",
			expectGetDNSAddressCalled: true,
			dnsAddressResponse: &dnsmgrpb.GetDNSAddressResponse{
				DNSAddress: "abc.clusters.dev.withpixie.dev",
			},
			dnsAddressError:        nil,
			vizierID:               "123e4567-e89b-12d3-a456-426655440001",
			hbAddress:              "127.0.0.1",
			hbPort:                 123,
			updatedClusterStatus:   "UPDATING",
			expectedClusterAddress: "abc.clusters.dev.withpixie.dev:123",
			status:                 cvmsgspb.VZ_ST_UPDATING,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			dnsMgrReq := &dnsmgrpb.GetDNSAddressRequest{
				ClusterID: utils.ProtoFromUUIDStrOrNil(tc.vizierID),
				IPAddress: tc.hbAddress,
			}

			if tc.expectGetDNSAddressCalled {
				mockDNSClient.EXPECT().
					GetDNSAddress(gomock.Any(), dnsMgrReq).
					Return(tc.dnsAddressResponse, tc.dnsAddressError)
			}

			if tc.expectedFetchVizierVersion {
				atReq := &artifacttrackerpb.GetArtifactListRequest{
					ArtifactName: "vizier",
					ArtifactType: versionspb.AT_CONTAINER_SET_YAMLS,
					Limit:        1,
				}
				mockArtifactTrackerClient.EXPECT().GetArtifactList(
					gomock.Any(), atReq).Return(&versionspb.ArtifactSet{
					Name: "vizier",
					Artifact: []*versionspb.Artifact{&versionspb.Artifact{
						VersionStr: "test",
					}},
				}, nil)
			}

			nestedMsg := &cvmsgspb.VizierHeartbeat{
				VizierID:         utils.ProtoFromUUIDStrOrNil(tc.vizierID),
				Time:             100,
				SequenceNumber:   200,
				Address:          tc.hbAddress,
				Port:             int32(tc.hbPort),
				Status:           tc.status,
				BootstrapMode:    tc.bootstrap,
				BootstrapVersion: tc.bootstrapVersion,
			}
			nestedAny, err := types.MarshalAny(nestedMsg)
			if err != nil {
				t.Fatal("Could not marshal pb")
			}

			req := &cvmsgspb.V2CMessage{
				Msg: nestedAny,
			}

			s.HandleVizierHeartbeat(req)

			// Check database.
			clusterQuery := `SELECT status, address from vizier_cluster_info WHERE vizier_cluster_id=$1`
			var clusterInfo struct {
				Status  string `db:"status"`
				Address string `db:"address"`
			}
			clusterID, err := uuid.FromString(tc.vizierID)
			assert.Nil(t, err)
			err = db.Get(&clusterInfo, clusterQuery, clusterID)
			assert.Equal(t, tc.updatedClusterStatus, clusterInfo.Status)
			assert.Equal(t, tc.expectedClusterAddress, clusterInfo.Address)
		})
	}
}

func TestServer_GetSSLCerts(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()

	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	subCh := make(chan *nats.Msg, 1)
	sub, err := nc.ChanSubscribe("c2v.123e4567-e89b-12d3-a456-426655440001.sslResp", subCh)
	if err != nil {
		t.Fatal("Could not subscribe to NATS.")
	}
	defer sub.Unsubscribe()
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)

	t.Run("dnsmgr error", func(t *testing.T) {
		dnsMgrReq := &dnsmgrpb.GetSSLCertsRequest{
			ClusterID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		}

		dnsMgrResp := &dnsmgrpb.GetSSLCertsResponse{
			Key:  "abcd",
			Cert: "efgh",
		}

		mockDNSClient.EXPECT().
			GetSSLCerts(gomock.Any(), dnsMgrReq).
			Return(dnsMgrResp, nil)

		nestedMsg := &cvmsgspb.VizierSSLCertRequest{
			VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
		}
		nestedAny, err := types.MarshalAny(nestedMsg)
		if err != nil {
			t.Fatal("Could not marshal pb")
		}

		req := &cvmsgspb.V2CMessage{
			Msg: nestedAny,
		}

		s.HandleSSLRequest(req)

		// Listen for response.
		select {
		case m := <-subCh:
			pb := &cvmsgspb.C2VMessage{}
			err = proto.Unmarshal(m.Data, pb)
			if err != nil {
				t.Fatal("Could not unmarshal message")
			}
			resp := &cvmsgspb.VizierSSLCertResponse{}
			err = types.UnmarshalAny(pb.Msg, resp)
			if err != nil {
				t.Fatal("Could not unmarshal any message")
			}
			assert.Equal(t, "abcd", resp.Key)
			assert.Equal(t, "efgh", resp.Cert)
		case <-time.After(1 * time.Second):
			t.Fatal("Timeout")
		}
	})
}

func TestServer_UpdateOrInstallVizier(t *testing.T) {
	viper.Set("jwt_signing_key", "jwtkey")

	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()

	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	vizierID, _ := uuid.FromString("123e4567-e89b-12d3-a456-426655440001")

	go nc.Subscribe("c2v.123e4567-e89b-12d3-a456-426655440001.VizierUpdate", func(m *nats.Msg) {
		c2vMsg := &cvmsgspb.C2VMessage{}
		err := proto.Unmarshal(m.Data, c2vMsg)
		assert.Nil(t, err)
		resp := &cvmsgspb.UpdateOrInstallVizierRequest{}
		err = types.UnmarshalAny(c2vMsg.Msg, resp)
		assert.Equal(t, "0.1.30", resp.Version)
		assert.NotNil(t, resp.Token)
		claims := jwt.MapClaims{}
		_, err = jwt.ParseWithClaims(resp.Token, claims, func(token *jwt.Token) (interface{}, error) {
			return []byte("jwtkey"), nil
		})
		assert.Nil(t, err)
		assert.Equal(t, "user", claims["Scopes"].(string))
		// Send response.
		updateResp := &cvmsgspb.UpdateOrInstallVizierResponse{
			UpdateStarted: true,
		}
		respAnyMsg, err := types.MarshalAny(updateResp)
		assert.Nil(t, err)
		wrappedMsg := &cvmsgspb.V2CMessage{
			VizierID: vizierID.String(),
			Msg:      respAnyMsg,
		}

		b, err := wrappedMsg.Marshal()
		assert.Nil(t, err)
		topic := vzshard.V2CTopic("VizierUpdateResponse", vizierID)
		err = nc.Publish(topic, b)
		assert.Nil(t, err)
	})
	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	s := controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)

	req := &cvmsgspb.UpdateOrInstallVizierRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil(vizierID.String()),
		Version:  "0.1.30",
	}

	resp, err := s.UpdateOrInstallVizier(CreateTestContext(), req)
	require.Nil(t, err)
	require.NotNil(t, resp)
}

func TestServer_MessageHandler(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()
	mockDNSClient := mock_dnsmgrpb.NewMockDNSMgrServiceClient(ctrl)

	port, cleanup := testingutils.StartNATS(t)
	defer cleanup()

	nc, err := nats.Connect(testingutils.GetNATSURL(port))
	if err != nil {
		t.Fatal("Could not connect to NATS.")
	}

	subCh := make(chan *nats.Msg, 1)
	sub, err := nc.ChanSubscribe("c2v.123e4567-e89b-12d3-a456-426655440001.sslResp", subCh)
	if err != nil {
		t.Fatal("Could not subscribe to NATS.")
	}
	defer sub.Unsubscribe()

	mockArtifactTrackerClient := mock_artifacttrackerpb.NewMockArtifactTrackerClient(ctrl)

	_ = controller.New(db, "test", mockDNSClient, mockArtifactTrackerClient, nc)

	dnsMgrReq := &dnsmgrpb.GetSSLCertsRequest{
		ClusterID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
	}
	dnsMgrResp := &dnsmgrpb.GetSSLCertsResponse{
		Key:  "abcd",
		Cert: "efgh",
	}

	mockDNSClient.EXPECT().
		GetSSLCerts(gomock.Any(), dnsMgrReq).
		Return(dnsMgrResp, nil)

	// Publish v2c message over nats.
	nestedMsg := &cvmsgspb.VizierSSLCertRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440001"),
	}
	nestedAny, err := types.MarshalAny(nestedMsg)
	if err != nil {
		t.Fatal("Could not marshal pb")
	}
	wrappedMsg := &cvmsgspb.V2CMessage{
		VizierID: "123e4567-e89b-12d3-a456-426655440001",
		Msg:      nestedAny,
	}

	b, err := wrappedMsg.Marshal()
	if err != nil {
		t.Fatal("Could not marshal message to bytes")
	}
	nc.Publish("v2c.1.123e4567-e89b-12d3-a456-426655440001.ssl", b)

	select {
	case m := <-subCh:
		pb := &cvmsgspb.C2VMessage{}
		err = proto.Unmarshal(m.Data, pb)
		if err != nil {
			t.Fatal("Could not unmarshal message")
		}
		resp := &cvmsgspb.VizierSSLCertResponse{}
		err = types.UnmarshalAny(pb.Msg, resp)
		if err != nil {
			t.Fatal("Could not unmarshal any message")
		}
		assert.Equal(t, "abcd", resp.Key)
		assert.Equal(t, "efgh", resp.Cert)
	case <-time.After(1 * time.Second):
		t.Fatal("Timeout")
	}
}

func TestServer_GetViziersByShard(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)

	tests := []struct {
		name string

		// Inputs:
		shardID string

		// Outputs:
		expectGRPCError error
		expectResponse  *vzmgrpb.GetViziersByShardResponse
	}{
		{
			name:            "Bad shardID",
			shardID:         "gf",
			expectGRPCError: status.Error(codes.InvalidArgument, "bad arg"),
		},
		{
			name:            "Bad shardID (valid hex, too large)",
			shardID:         "fff",
			expectGRPCError: status.Error(codes.InvalidArgument, "bad arg"),
		},
		{
			name:            "Bad shardID (valid hex, too small)",
			shardID:         "f",
			expectGRPCError: status.Error(codes.InvalidArgument, "bad arg"),
		},
		{
			name:    "Single vizier response",
			shardID: "00",

			expectGRPCError: nil,
			expectResponse: &vzmgrpb.GetViziersByShardResponse{
				Viziers: []*vzmgrpb.GetViziersByShardResponse_VizierInfo{
					{
						VizierID: utils.ProtoFromUUIDStrOrNil("123e4567-e89b-12d3-a456-426655440000"),
						OrgID:    utils.ProtoFromUUIDStrOrNil("223e4567-e89b-12d3-a456-426655440000"),
					},
				},
			},
		},
		{
			name:    "Multi vizier response",
			shardID: "03",

			expectGRPCError: nil,
			expectResponse: &vzmgrpb.GetViziersByShardResponse{
				Viziers: []*vzmgrpb.GetViziersByShardResponse_VizierInfo{
					{
						VizierID: utils.ProtoFromUUIDStrOrNil("323e4567-e89b-12d3-a456-426655440003"),
						OrgID:    utils.ProtoFromUUIDStrOrNil("223e4567-e89b-12d3-a456-426655440001"),
					},
					{
						VizierID: utils.ProtoFromUUIDStrOrNil("223e4567-e89b-12d3-a456-426655440003"),
						OrgID:    utils.ProtoFromUUIDStrOrNil("223e4567-e89b-12d3-a456-426655440001"),
					},
				},
			},
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			req := &vzmgrpb.GetViziersByShardRequest{ShardID: test.shardID}
			resp, err := s.GetViziersByShard(context.Background(), req)

			if test.expectGRPCError != nil {
				assert.Equal(t, status.Code(test.expectGRPCError), status.Code(err))
			} else {
				assert.Equal(t, test.expectResponse, resp)
			}
		})
	}
}

func TestServer_ProvisionOrClaimVizier(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)
	// TODO(zasgar): We need to make user IDS make sense.
	userID := uuid.NewV4()

	// This should select the first cluster with an empty UID that is disconnected.
	clusterID, err := s.ProvisionOrClaimVizier(context.Background(), uuid.FromStringOrNil(testAuthOrgID), userID, "my cluster", "", "1.1")
	assert.Nil(t, err)
	// Should select the disconnected cluster.
	assert.Equal(t, testDisconnectedClusterEmptyUID, clusterID.String())
}

func TestServer_ProvisionOrClaimVizier_WithExistingUID(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)
	userID := uuid.NewV4()

	// This should select the existing cluster with the same UID.
	clusterID, err := s.ProvisionOrClaimVizier(context.Background(), uuid.FromStringOrNil(testAuthOrgID), userID, "existing_cluster", "", "1.1")
	assert.Nil(t, err)
	// Should select the disconnected cluster.
	assert.Equal(t, testExistingCluster, clusterID.String())
}

func TestServer_ProvisionOrClaimVizier_WithExistingActiveUID(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)
	userID := uuid.NewV4()
	// This should select cause an error b/c we are trying to provision a cluster that is not disconnected.
	clusterID, err := s.ProvisionOrClaimVizier(context.Background(), uuid.FromStringOrNil(testAuthOrgID), userID, "my_other_cluster", "", "1.1")
	assert.NotNil(t, err)
	assert.Equal(t, vzerrors.ErrProvisionFailedVizierIsActive, err)
	assert.Equal(t, uuid.Nil, clusterID)
}

func TestServer_ProvisionOrClaimVizier_WithNewCluster(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)
	userID := uuid.NewV4()
	// This should select cause an error b/c we are trying to provision a cluster that is not disconnected.
	clusterID, err := s.ProvisionOrClaimVizier(context.Background(), uuid.FromStringOrNil(testNonAuthOrgID), userID, "my_other_cluster", "", "1.1")
	assert.Nil(t, err)
	assert.NotEqual(t, uuid.Nil, clusterID)
}

func TestServer_ProvisionOrClaimVizier_WithExistingName(t *testing.T) {
	db, teardown := setupTestDB(t)
	defer teardown()
	loadTestData(t, db)

	s := controller.New(db, "test", nil, nil, nil)
	userID := uuid.NewV4()

	// This should select the existing cluster with the same UID.
	clusterID, err := s.ProvisionOrClaimVizier(context.Background(), uuid.FromStringOrNil(testAuthOrgID), userID, "some_cluster", "test-cluster\n", "1.1")
	assert.Nil(t, err)
	// Should select the disconnected cluster.
	assert.Equal(t, testDisconnectedClusterEmptyUID, clusterID.String())
	// Check cluster name.
	var clusterInfo struct {
		ClusterName    *string `db:"cluster_name"`
		ClusterVersion *string `db:"cluster_version"`
	}
	nameQuery := `SELECT cluster_name, cluster_version from vizier_cluster WHERE id=$1`
	err = db.Get(&clusterInfo, nameQuery, clusterID)
	assert.Nil(t, err)
	assert.Equal(t, "test-cluster_1", *clusterInfo.ClusterName)
	assert.Equal(t, "1.1", *clusterInfo.ClusterVersion)
}
