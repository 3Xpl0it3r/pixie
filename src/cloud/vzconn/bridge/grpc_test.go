package bridge_test

import (
	"context"
	"fmt"
	"net"
	"testing"
	"time"

	"github.com/gogo/protobuf/proto"
	"github.com/gogo/protobuf/types"
	"github.com/golang/mock/gomock"
	"github.com/nats-io/nats.go"
	"github.com/nats-io/stan.go"
	uuid "github.com/satori/go.uuid"
	"github.com/stretchr/testify/assert"
	"google.golang.org/genproto/googleapis/rpc/code"
	"google.golang.org/grpc"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
	"pixielabs.ai/pixielabs/src/cloud/shared/vzshard"
	"pixielabs.ai/pixielabs/src/cloud/vzconn/bridge"
	"pixielabs.ai/pixielabs/src/cloud/vzconn/vzconnpb"
	"pixielabs.ai/pixielabs/src/cloud/vzmgr/vzmgrpb"
	mock_vzmgrpb "pixielabs.ai/pixielabs/src/cloud/vzmgr/vzmgrpb/mock"
	"pixielabs.ai/pixielabs/src/shared/cvmsgspb"
	"pixielabs.ai/pixielabs/src/utils"

	"pixielabs.ai/pixielabs/src/utils/testingutils"
)

const bufSize = 1024 * 1024

type testState struct {
	t   *testing.T
	lis *bufconn.Listener
	b   *bridge.GRPCServer

	nc               *nats.Conn
	sc               stan.Conn
	conn             *grpc.ClientConn
	mockVZMgr        *mock_vzmgrpb.MockVZMgrServiceClient
	mockVZDeployment *mock_vzmgrpb.MockVZDeploymentServiceClient
}

func createTestState(t *testing.T, ctrl *gomock.Controller) (*testState, func(t *testing.T)) {
	lis := bufconn.Listen(bufSize)
	s := grpc.NewServer()
	mockVZMgr := mock_vzmgrpb.NewMockVZMgrServiceClient(ctrl)
	mockVZDeployment := mock_vzmgrpb.NewMockVZDeploymentServiceClient(ctrl)

	natsPort, natsCleanup := testingutils.StartNATS(t)
	nc, err := nats.Connect(testingutils.GetNATSURL(natsPort))
	if err != nil {
		t.Fatal(err)
	}

	_, sc, cleanStan := testingutils.StartStan(t, "test-stan", "test-client")
	b := bridge.NewBridgeGRPCServer(mockVZMgr, mockVZDeployment, nc, sc)
	vzconnpb.RegisterVZConnServiceServer(s, b)

	go func() {
		if err := s.Serve(lis); err != nil {
			t.Fatalf("Server exited with error: %v\n", err)
		}
	}()

	ctx := context.Background()
	conn, err := grpc.DialContext(ctx, "bufnet", grpc.WithDialer(createDialer(lis)), grpc.WithInsecure())
	if err != nil {
		t.Fatalf("Failed to dial bufnet: %v", err)
	}

	cleanupFunc := func(t *testing.T) {
		natsCleanup()
		conn.Close()
		cleanStan()
	}

	return &testState{
		t:                t,
		lis:              lis,
		b:                b,
		nc:               nc,
		conn:             conn,
		mockVZMgr:        mockVZMgr,
		mockVZDeployment: mockVZDeployment,
	}, cleanupFunc
}

func createDialer(lis *bufconn.Listener) func(string, time.Duration) (net.Conn, error) {
	return func(str string, duration time.Duration) (conn net.Conn, e error) {
		return lis.Dial()
	}
}

type readMsgWrapper struct {
	msg *vzconnpb.C2VBridgeMessage
	err error
}

func grpcReader(stream vzconnpb.VZConnService_NATSBridgeClient) chan readMsgWrapper {
	c := make(chan readMsgWrapper)
	go func() {
		for {
			select {
			case <-stream.Context().Done():
				return
			default:
				m, err := stream.Recv()
				c <- readMsgWrapper{
					msg: m,
					err: err,
				}
			}
		}
	}()
	return c
}

func convertToAny(msg proto.Message) *types.Any {
	any, err := types.MarshalAny(msg)
	if err != nil {
		panic(err)
	}
	return any
}

func TestNATSGRPCBridgeHandshakeTest_CorrectRegistration(t *testing.T) {
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	// Make some GRPC Requests.
	ctx := context.Background()
	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	stream, err := client.NATSBridge(ctx)
	if err != nil {
		t.Fatal(err)
	}

	readCh := grpcReader(stream)
	vizierID := uuid.NewV4()
	regReq := &cvmsgspb.RegisterVizierRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil(vizierID.String()),
		JwtKey:   "123",
		Address:  "123:123",
	}

	err = stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "register",
		SessionId: 0,
		Msg:       convertToAny(regReq),
	})

	ts.mockVZMgr.EXPECT().
		VizierConnected(gomock.Any(), regReq).
		Return(&cvmsgspb.RegisterVizierAck{Status: cvmsgspb.ST_OK}, nil)

	// Should get the register ACK.
	m := <-readCh
	assert.Nil(t, m.err)
	assert.Equal(t, "registerAck", m.msg.Topic)
	ack := cvmsgspb.RegisterVizierAck{}
	err = types.UnmarshalAny(m.msg.Msg, &ack)
	if err != nil {
		t.Fatal("Expected to get back RegisterVizierAck message")
	}

	assert.Equal(t, cvmsgspb.ST_OK, ack.Status)
}

func TestNATSGRPCBridgeHandshakeTest_MissingRegister(t *testing.T) {
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	// Make some GRPC Requests.
	ctx := context.Background()
	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	stream, err := client.NATSBridge(ctx)
	if err != nil {
		t.Fatal(err)
	}

	readCh := grpcReader(stream)
	err = stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "not-register",
		SessionId: 0,
		Msg:       nil,
	})

	// Should get the register error.
	m := <-readCh
	assert.NotNil(t, m.err)
	assert.NotNil(t, code.Code_INVALID_ARGUMENT, status.Code(m.err))
	assert.Nil(t, m.msg)
}

func TestNATSGRPCBridgeHandshakeTest_NilRegister(t *testing.T) {
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	// Make some GRPC Requests.
	ctx := context.Background()
	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	stream, err := client.NATSBridge(ctx)
	if err != nil {
		t.Fatal(err)
	}

	readCh := grpcReader(stream)
	err = stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "register",
		SessionId: 0,
		Msg:       nil,
	})

	// Should get the register error.
	m := <-readCh
	assert.NotNil(t, m.err)
	assert.NotNil(t, code.Code_INVALID_ARGUMENT, status.Code(m.err))
	assert.Nil(t, m.msg)
}

func TestNATSGRPCBridgeHandshakeTest_MalformedRegister(t *testing.T) {
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	// Make some GRPC Requests.
	ctx := context.Background()
	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	stream, err := client.NATSBridge(ctx)
	if err != nil {
		t.Fatal(err)
	}

	readCh := grpcReader(stream)
	err = stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "register",
		SessionId: 0,
		Msg:       convertToAny(&cvmsgspb.RegisterVizierAck{}),
	})

	// Should get the register error.
	m := <-readCh
	assert.NotNil(t, m.err)
	assert.NotNil(t, code.Code_INVALID_ARGUMENT, status.Code(m.err))
	assert.Nil(t, m.msg)
}

func registerVizier(ts *testState, vizierID uuid.UUID, stream vzconnpb.VZConnService_NATSBridgeClient, readCh chan readMsgWrapper) {
	regReq := &cvmsgspb.RegisterVizierRequest{
		VizierID: utils.ProtoFromUUIDStrOrNil(vizierID.String()),
		JwtKey:   "123",
		Address:  "123:123",
	}

	err := stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "register",
		SessionId: 0,
		Msg:       convertToAny(regReq),
	})

	ts.mockVZMgr.EXPECT().
		VizierConnected(gomock.Any(), regReq).
		Return(&cvmsgspb.RegisterVizierAck{Status: cvmsgspb.ST_OK}, nil)

	// Should get the register ACK.
	m := <-readCh
	assert.Nil(ts.t, m.err)
	assert.Equal(ts.t, "registerAck", m.msg.Topic)
	ack := cvmsgspb.RegisterVizierAck{}
	err = types.UnmarshalAny(m.msg.Msg, &ack)
	if err != nil {
		ts.t.Fatal("Expected to get back RegisterVizierAck message")
	}

	assert.Equal(ts.t, cvmsgspb.ST_OK, ack.Status)
}

func TestNATSGRPCBridge_BridgingTest(t *testing.T) {
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	// Make some GRPC Requests.
	ctx := context.Background()
	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	stream, err := client.NATSBridge(ctx)
	if err != nil {
		t.Fatal(err)
	}

	readCh := grpcReader(stream)
	vizierID := uuid.NewV4()
	registerVizier(ts, vizierID, stream, readCh)

	t1Ch := make(chan *nats.Msg, 10)
	topic := vzshard.V2CTopic("t1", vizierID)
	sub, err := ts.nc.ChanSubscribe(topic, t1Ch)
	assert.Nil(t, err)
	defer sub.Unsubscribe()

	fmt.Printf("Listing to topic: %s\n", topic)
	// Send a message on t1 and expect it show up on t1Ch
	err = stream.Send(&vzconnpb.V2CBridgeMessage{
		Topic:     "t1",
		SessionId: 0,
		Msg: convertToAny(&cvmsgspb.VizierHeartbeat{
			VizierID: utils.ProtoFromUUIDStrOrNil(vizierID.String()),
		}),
	})

	assert.Nil(t, err)

	natsMsg := <-t1Ch

	expectedMsg := &cvmsgspb.V2CMessage{
		VizierID: vizierID.String(),
		Msg: convertToAny(&cvmsgspb.VizierHeartbeat{
			VizierID: utils.ProtoFromUUIDStrOrNil(vizierID.String()),
		}),
	}

	msg := &cvmsgspb.V2CMessage{}
	err = msg.Unmarshal(natsMsg.Data)
	assert.Nil(t, err)
	assert.Equal(t, expectedMsg, msg)
}

func TestNATSGRPCBridge_RegisterVizierDeployment(t *testing.T) {
	vizierID := uuid.NewV4()
	ctrl := gomock.NewController(t)
	ts, cleanup := createTestState(t, ctrl)
	defer cleanup(t)

	ts.mockVZDeployment.EXPECT().
		RegisterVizierDeployment(gomock.Any(), &vzmgrpb.RegisterVizierDeploymentRequest{
			K8sClusterUID: "test",
			DeploymentKey: "deploy-key",
		}).
		Return(&vzmgrpb.RegisterVizierDeploymentResponse{VizierID: utils.ProtoFromUUID(&vizierID)}, nil)

	// Make some GRPC Requests.
	ctx := context.Background()
	ctx = metadata.AppendToOutgoingContext(ctx, "X-API-KEY", "deploy-key")

	client := vzconnpb.NewVZConnServiceClient(ts.conn)
	resp, err := client.RegisterVizierDeployment(ctx, &vzconnpb.RegisterVizierDeploymentRequest{
		K8sClusterUID: "test",
	})
	assert.Nil(t, err)
	assert.Equal(t, utils.ProtoFromUUID(&vizierID), resp.VizierID)

}

// TODO(zasgar/michelle): Add tests for disconnect of Vizier. Should make sure messages are not lost on durable channel.
// TODO(zasgar/michelle): Add tests for disconnect of vizier. Should make sure go routines are not leaked.
