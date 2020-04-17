package controller_test

import (
	"context"
	"reflect"
	"testing"

	"github.com/gogo/protobuf/proto"
	types "github.com/gogo/protobuf/types"
	"github.com/golang/mock/gomock"
	uuid "github.com/satori/go.uuid"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"pixielabs.ai/pixielabs/src/cloud/api/controller"
	"pixielabs.ai/pixielabs/src/cloud/api/controller/testutils"
	artifacttrackerpb "pixielabs.ai/pixielabs/src/cloud/artifact_tracker/artifacttrackerpb"
	"pixielabs.ai/pixielabs/src/cloud/autocomplete"
	mock_autocomplete "pixielabs.ai/pixielabs/src/cloud/autocomplete/mock"
	"pixielabs.ai/pixielabs/src/cloud/cloudapipb"
	"pixielabs.ai/pixielabs/src/cloud/scriptmgr/scriptmgrpb"
	mock_scriptmgr "pixielabs.ai/pixielabs/src/cloud/scriptmgr/scriptmgrpb/mock"
	vzmgrpb "pixielabs.ai/pixielabs/src/cloud/vzmgr/vzmgrpb"
	uuidpb "pixielabs.ai/pixielabs/src/common/uuid/proto"
	versionspb "pixielabs.ai/pixielabs/src/shared/artifacts/versionspb"
	"pixielabs.ai/pixielabs/src/shared/cvmsgspb"
	pl_vispb "pixielabs.ai/pixielabs/src/shared/vispb"
	pbutils "pixielabs.ai/pixielabs/src/utils"
)

func TestArtifactTracker_GetArtifactList(t *testing.T) {
	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, _, mockArtifactClient, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := context.Background()

	mockArtifactClient.EXPECT().GetArtifactList(gomock.Any(),
		&artifacttrackerpb.GetArtifactListRequest{
			ArtifactName: "cli",
			Limit:        1,
			ArtifactType: versionspb.AT_LINUX_AMD64,
		}).
		Return(&versionspb.ArtifactSet{
			Name: "cli",
			Artifact: []*versionspb.Artifact{&versionspb.Artifact{
				VersionStr: "test",
			}},
		}, nil)

	artifactTrackerServer := &controller.ArtifactTrackerServer{
		ArtifactTrackerClient: mockArtifactClient,
	}

	resp, err := artifactTrackerServer.GetArtifactList(ctx, &cloudapipb.GetArtifactListRequest{
		ArtifactName: "cli",
		Limit:        1,
		ArtifactType: cloudapipb.AT_LINUX_AMD64,
	})

	assert.Nil(t, err)
	assert.Equal(t, "cli", resp.Name)
	assert.Equal(t, 1, len(resp.Artifact))
}

func TestArtifactTracker_GetDownloadLink(t *testing.T) {
	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, _, mockArtifactClient, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := context.Background()

	mockArtifactClient.EXPECT().GetDownloadLink(gomock.Any(),
		&artifacttrackerpb.GetDownloadLinkRequest{
			ArtifactName: "cli",
			VersionStr:   "version",
			ArtifactType: versionspb.AT_LINUX_AMD64,
		}).
		Return(&artifacttrackerpb.GetDownloadLinkResponse{
			Url:    "http://localhost",
			SHA256: "sha",
		}, nil)

	artifactTrackerServer := &controller.ArtifactTrackerServer{
		ArtifactTrackerClient: mockArtifactClient,
	}

	resp, err := artifactTrackerServer.GetDownloadLink(ctx, &cloudapipb.GetDownloadLinkRequest{
		ArtifactName: "cli",
		VersionStr:   "version",
		ArtifactType: cloudapipb.AT_LINUX_AMD64,
	})

	assert.Nil(t, err)
	assert.Equal(t, "http://localhost", resp.Url)
	assert.Equal(t, "sha", resp.SHA256)
}

func TestVizierClusterInfo_CreateCluster(t *testing.T) {
	orgID := pbutils.ProtoFromUUIDStrOrNil("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
	clusterID := pbutils.ProtoFromUUIDStrOrNil("7ba7b810-9dad-11d1-80b4-00c04fd430c8")
	assert.NotNil(t, clusterID)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, mockVzMgr, _, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := CreateTestContext()

	ccReq := &vzmgrpb.CreateVizierClusterRequest{
		OrgID: orgID,
	}
	mockVzMgr.EXPECT().CreateVizierCluster(gomock.Any(), ccReq).Return(clusterID, nil)

	vzClusterInfoServer := &controller.VizierClusterInfo{
		VzMgr: mockVzMgr,
	}

	resp, err := vzClusterInfoServer.CreateCluster(ctx, &cloudapipb.CreateClusterRequest{})
	assert.Nil(t, err)
	assert.NotNil(t, resp)
	assert.Equal(t, resp.ClusterID, clusterID)
}

func TestVizierClusterInfo_GetClusterInfo(t *testing.T) {
	orgID := pbutils.ProtoFromUUIDStrOrNil("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
	clusterID := pbutils.ProtoFromUUIDStrOrNil("7ba7b810-9dad-11d1-80b4-00c04fd430c8")
	assert.NotNil(t, clusterID)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, mockVzMgr, _, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := CreateTestContext()

	mockVzMgr.EXPECT().GetViziersByOrg(gomock.Any(), orgID).Return(&vzmgrpb.GetViziersByOrgResponse{
		VizierIDs: []*uuidpb.UUID{clusterID},
	}, nil)

	mockVzMgr.EXPECT().GetVizierInfo(gomock.Any(), clusterID).Return(&cvmsgspb.VizierInfo{
		VizierID:        clusterID,
		Status:          cvmsgspb.VZ_ST_HEALTHY,
		LastHeartbeatNs: int64(1305646598000000000),
		Config: &cvmsgspb.VizierConfig{
			PassthroughEnabled: false,
		},
	}, nil)

	vzClusterInfoServer := &controller.VizierClusterInfo{
		VzMgr: mockVzMgr,
	}

	resp, err := vzClusterInfoServer.GetClusterInfo(ctx, &cloudapipb.GetClusterInfoRequest{})

	assert.Nil(t, err)
	assert.Equal(t, 1, len(resp.Clusters))
	cluster := resp.Clusters[0]
	assert.Equal(t, cluster.ID, clusterID)
	assert.Equal(t, cluster.Status, cloudapipb.CS_HEALTHY)
	assert.Equal(t, cluster.LastHeartbeatNs, int64(1305646598000000000))
	assert.Equal(t, cluster.Config.PassthroughEnabled, false)
}

func TestVizierClusterInfo_GetClusterInfoWithID(t *testing.T) {
	clusterID := pbutils.ProtoFromUUIDStrOrNil("7ba7b810-9dad-11d1-80b4-00c04fd430c8")
	assert.NotNil(t, clusterID)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, mockVzMgr, _, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := CreateTestContext()

	mockVzMgr.EXPECT().GetVizierInfo(gomock.Any(), clusterID).Return(&cvmsgspb.VizierInfo{
		VizierID:        clusterID,
		Status:          cvmsgspb.VZ_ST_HEALTHY,
		LastHeartbeatNs: int64(1305646598000000000),
		Config: &cvmsgspb.VizierConfig{
			PassthroughEnabled: false,
		},
	}, nil)

	vzClusterInfoServer := &controller.VizierClusterInfo{
		VzMgr: mockVzMgr,
	}

	resp, err := vzClusterInfoServer.GetClusterInfo(ctx, &cloudapipb.GetClusterInfoRequest{
		ID: clusterID,
	})

	assert.Nil(t, err)
	assert.Equal(t, 1, len(resp.Clusters))
	cluster := resp.Clusters[0]
	assert.Equal(t, cluster.ID, clusterID)
	assert.Equal(t, cluster.Status, cloudapipb.CS_HEALTHY)
	assert.Equal(t, cluster.LastHeartbeatNs, int64(1305646598000000000))
	assert.Equal(t, cluster.Config.PassthroughEnabled, false)
}

func TestVizierClusterInfo_UpdateClusterVizierConfig(t *testing.T) {
	clusterID := pbutils.ProtoFromUUIDStrOrNil("7ba7b810-9dad-11d1-80b4-00c04fd430c8")
	assert.NotNil(t, clusterID)

	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	_, _, _, mockVzMgr, _, cleanup := testutils.CreateTestAPIEnv(t)
	defer cleanup()
	ctx := CreateTestContext()

	updateReq := &cvmsgspb.UpdateVizierConfigRequest{
		VizierID: clusterID,
		ConfigUpdate: &cvmsgspb.VizierConfigUpdate{
			PassthroughEnabled: &types.BoolValue{Value: true},
		},
	}

	mockVzMgr.EXPECT().UpdateVizierConfig(gomock.Any(), updateReq).Return(&cvmsgspb.UpdateVizierConfigResponse{}, nil)

	vzClusterInfoServer := &controller.VizierClusterInfo{
		VzMgr: mockVzMgr,
	}

	resp, err := vzClusterInfoServer.UpdateClusterVizierConfig(ctx, &cloudapipb.UpdateClusterVizierConfigRequest{
		ID: clusterID,
		ConfigUpdate: &cloudapipb.VizierConfigUpdate{
			PassthroughEnabled: &types.BoolValue{Value: true},
		},
	})

	assert.Nil(t, err)
	assert.NotNil(t, resp)
}

type SuggestionRequest struct {
	requestKinds []cloudapipb.AutocompleteEntityKind
	requestArgs  []cloudapipb.AutocompleteEntityKind
	suggestions  []*autocomplete.Suggestion
}

func TestAutocompleteService_Autocomplete(t *testing.T) {
	ctrl := gomock.NewController(t)
	defer ctrl.Finish()

	orgID, err := uuid.FromString("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
	assert.Nil(t, err)
	ctx := CreateTestContext()

	s := mock_autocomplete.NewMockSuggester(ctrl)

	requests := [][]*autocomplete.SuggestionRequest{
		[]*autocomplete.SuggestionRequest{
			&autocomplete.SuggestionRequest{
				OrgID:        orgID,
				Input:        "px/svc_info",
				AllowedKinds: []cloudapipb.AutocompleteEntityKind{cloudapipb.AEK_POD, cloudapipb.AEK_SVC, cloudapipb.AEK_NAMESPACE, cloudapipb.AEK_SCRIPT},
				AllowedArgs:  []cloudapipb.AutocompleteEntityKind{},
			},
			&autocomplete.SuggestionRequest{
				OrgID:        orgID,
				Input:        "pl/test",
				AllowedKinds: []cloudapipb.AutocompleteEntityKind{cloudapipb.AEK_POD, cloudapipb.AEK_SVC, cloudapipb.AEK_NAMESPACE, cloudapipb.AEK_SCRIPT},
				AllowedArgs:  []cloudapipb.AutocompleteEntityKind{},
			},
		},
	}

	responses := [][]*autocomplete.SuggestionResult{
		[]*autocomplete.SuggestionResult{
			&autocomplete.SuggestionResult{
				Suggestions: []*autocomplete.Suggestion{
					&autocomplete.Suggestion{
						Name:  "px/svc_info",
						Score: 1,
						Args:  []cloudapipb.AutocompleteEntityKind{cloudapipb.AEK_SVC},
					},
				},
				ExactMatch: true,
			},
			&autocomplete.SuggestionResult{
				Suggestions: []*autocomplete.Suggestion{
					&autocomplete.Suggestion{
						Name:  "px/test",
						Score: 1,
					},
				},
				ExactMatch: true,
			},
		},
	}

	suggestionCalls := 0
	s.EXPECT().
		GetSuggestions(gomock.Any()).
		DoAndReturn(func(req []*autocomplete.SuggestionRequest) ([]*autocomplete.SuggestionResult, error) {
			assert.ElementsMatch(t, requests[suggestionCalls], req)
			resp := responses[suggestionCalls]
			suggestionCalls++
			return resp, nil
		}).
		Times(len(requests))

	autocompleteServer := &controller.AutocompleteServer{
		Suggester: s,
	}

	resp, err := autocompleteServer.Autocomplete(ctx, &cloudapipb.AutocompleteRequest{
		Input:     "px/svc_info pl/test",
		CursorPos: 0,
		Action:    cloudapipb.AAT_EDIT,
	})
	assert.Nil(t, err)
	assert.NotNil(t, resp)
	assert.Equal(t, "${2:run} ${3:$0px/svc_info} ${1:pl/test}", resp.FormattedInput)
	assert.False(t, resp.IsExecutable)
	assert.Equal(t, 3, len(resp.TabSuggestions))
}

func toBytes(t *testing.T, msg proto.Message) []byte {
	bytes, err := proto.Marshal(msg)
	require.Nil(t, err)
	return bytes
}

func TestScriptMgr(t *testing.T) {
	var testVis = &pl_vispb.Vis{
		Widgets: []*pl_vispb.Widget{
			&pl_vispb.Widget{
				Func: &pl_vispb.Widget_Func{
					Name: "my_func",
				},
				DisplaySpec: &types.Any{
					TypeUrl: "pixielabs.ai/pl.vispb.VegaChart",
					Value: toBytes(t, &pl_vispb.VegaChart{
						Spec: "{}",
					}),
				},
			},
		},
	}

	ID1 := uuid.NewV4()
	ID2 := uuid.NewV4()
	testCases := []struct {
		name         string
		endpoint     string
		smReq        proto.Message
		smResp       proto.Message
		req          proto.Message
		expectedResp proto.Message
	}{
		{
			name:     "GetLiveViews correctly translates from scriptmgrpb to cloudapipb.",
			endpoint: "GetLiveViews",
			smReq:    &scriptmgrpb.GetLiveViewsReq{},
			smResp: &scriptmgrpb.GetLiveViewsResp{
				LiveViews: []*scriptmgrpb.LiveViewMetadata{
					&scriptmgrpb.LiveViewMetadata{
						ID:   pbutils.ProtoFromUUID(&ID1),
						Name: "liveview1",
						Desc: "liveview1 desc",
					},
					&scriptmgrpb.LiveViewMetadata{
						ID:   pbutils.ProtoFromUUID(&ID2),
						Name: "liveview2",
						Desc: "liveview2 desc",
					},
				},
			},
			req: &cloudapipb.GetLiveViewsReq{},
			expectedResp: &cloudapipb.GetLiveViewsResp{
				LiveViews: []*cloudapipb.LiveViewMetadata{
					&cloudapipb.LiveViewMetadata{
						ID:   ID1.String(),
						Name: "liveview1",
						Desc: "liveview1 desc",
					},
					&cloudapipb.LiveViewMetadata{
						ID:   ID2.String(),
						Name: "liveview2",
						Desc: "liveview2 desc",
					},
				},
			},
		},
		{
			name:     "GetLiveViewContents correctly translates between scriptmgr and cloudapipb.",
			endpoint: "GetLiveViewContents",
			smReq: &scriptmgrpb.GetLiveViewContentsReq{
				LiveViewID: pbutils.ProtoFromUUID(&ID1),
			},
			smResp: &scriptmgrpb.GetLiveViewContentsResp{
				Metadata: &scriptmgrpb.LiveViewMetadata{
					ID:   pbutils.ProtoFromUUID(&ID1),
					Name: "liveview1",
					Desc: "liveview1 desc",
				},
				PxlContents: "liveview1 pxl",
				Vis:         testVis,
			},
			req: &cloudapipb.GetLiveViewContentsReq{
				LiveViewID: ID1.String(),
			},
			expectedResp: &cloudapipb.GetLiveViewContentsResp{
				Metadata: &cloudapipb.LiveViewMetadata{
					ID:   ID1.String(),
					Name: "liveview1",
					Desc: "liveview1 desc",
				},
				PxlContents: "liveview1 pxl",
				Vis:         testVis,
			},
		},
		{
			name:     "GetScripts correctly translates between scriptmgr and cloudapipb.",
			endpoint: "GetScripts",
			smReq:    &scriptmgrpb.GetScriptsReq{},
			smResp: &scriptmgrpb.GetScriptsResp{
				Scripts: []*scriptmgrpb.ScriptMetadata{
					&scriptmgrpb.ScriptMetadata{
						ID:          pbutils.ProtoFromUUID(&ID1),
						Name:        "script1",
						Desc:        "script1 desc",
						HasLiveView: false,
					},
					&scriptmgrpb.ScriptMetadata{
						ID:          pbutils.ProtoFromUUID(&ID2),
						Name:        "liveview1",
						Desc:        "liveview1 desc",
						HasLiveView: true,
					},
				},
			},
			req: &cloudapipb.GetScriptsReq{},
			expectedResp: &cloudapipb.GetScriptsResp{
				Scripts: []*cloudapipb.ScriptMetadata{
					&cloudapipb.ScriptMetadata{
						ID:          ID1.String(),
						Name:        "script1",
						Desc:        "script1 desc",
						HasLiveView: false,
					},
					&cloudapipb.ScriptMetadata{
						ID:          ID2.String(),
						Name:        "liveview1",
						Desc:        "liveview1 desc",
						HasLiveView: true,
					},
				},
			},
		},
		{
			name:     "GetScriptContents correctly translates between scriptmgr and cloudapipb.",
			endpoint: "GetScriptContents",
			smReq: &scriptmgrpb.GetScriptContentsReq{
				ScriptID: pbutils.ProtoFromUUID(&ID1),
			},
			smResp: &scriptmgrpb.GetScriptContentsResp{
				Metadata: &scriptmgrpb.ScriptMetadata{
					ID:          pbutils.ProtoFromUUID(&ID1),
					Name:        "Script1",
					Desc:        "Script1 desc",
					HasLiveView: false,
				},
				Contents: "Script1 pxl",
			},
			req: &cloudapipb.GetScriptContentsReq{
				ScriptID: ID1.String(),
			},
			expectedResp: &cloudapipb.GetScriptContentsResp{
				Metadata: &cloudapipb.ScriptMetadata{
					ID:          ID1.String(),
					Name:        "Script1",
					Desc:        "Script1 desc",
					HasLiveView: false,
				},
				Contents: "Script1 pxl",
			},
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			ctrl := gomock.NewController(t)
			defer ctrl.Finish()

			mockScriptMgr := mock_scriptmgr.NewMockScriptMgrServiceClient(ctrl)
			ctx := CreateTestContext()

			reflect.ValueOf(mockScriptMgr.EXPECT()).
				MethodByName(tc.endpoint).
				Call([]reflect.Value{
					reflect.ValueOf(gomock.Any()),
					reflect.ValueOf(tc.smReq),
				})[0].Interface().(*gomock.Call).
				Return(tc.smResp, nil)

			scriptMgrServer := &controller.ScriptMgrServer{
				ScriptMgr: mockScriptMgr,
			}

			returnVals := reflect.ValueOf(scriptMgrServer).
				MethodByName(tc.endpoint).
				Call([]reflect.Value{
					reflect.ValueOf(ctx),
					reflect.ValueOf(tc.req),
				})
			assert.Nil(t, returnVals[1].Interface())
			resp := returnVals[0].Interface().(proto.Message)

			assert.Equal(t, tc.expectedResp, resp)
		})
	}
}
