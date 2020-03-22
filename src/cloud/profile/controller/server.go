package controller

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/badoux/checkmail"
	uuid "github.com/satori/go.uuid"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	"pixielabs.ai/pixielabs/src/cloud/profile/datastore"
	"pixielabs.ai/pixielabs/src/cloud/profile/profileenv"
	profile "pixielabs.ai/pixielabs/src/cloud/profile/profilepb"
	"pixielabs.ai/pixielabs/src/cloud/project_manager/projectmanagerpb"
	uuidpb "pixielabs.ai/pixielabs/src/common/uuid/proto"
	"pixielabs.ai/pixielabs/src/utils"
)

var emailDomainBlacklist = map[string]bool{
	"blacklist.com": true,
}

// DefaultProjectName is the name of the default project we automatically assign to every org.
const DefaultProjectName string = "default"

// Datastore is the interface used to the backing store for profile information.
type Datastore interface {
	// CreateUser creates a new user.
	CreateUser(*datastore.UserInfo) (uuid.UUID, error)
	// GetUser gets a user by ID.
	GetUser(uuid.UUID) (*datastore.UserInfo, error)
	// GetUserByEmail gets a user by email.
	GetUserByEmail(string) (*datastore.UserInfo, error)

	// CreateUserAndOrg creates a user and org for creating a new org with specified user as owner.
	CreateUserAndOrg(*datastore.OrgInfo, *datastore.UserInfo) (orgID uuid.UUID, userID uuid.UUID, err error)
	// GetOrg gets and org by ID.
	GetOrg(uuid.UUID) (*datastore.OrgInfo, error)
	// GetOrgByDomain gets an org by domain name.
	GetOrgByDomain(string) (*datastore.OrgInfo, error)
	// Delete Org and all of its users
	DeleteOrgAndUsers(uuid.UUID) error
}

// Server is an implementation of GRPC server for profile service.
type Server struct {
	env profileenv.ProfileEnv
	d   Datastore
}

// NewServer creates a new GRPC profile server.
func NewServer(env profileenv.ProfileEnv, d Datastore) *Server {
	return &Server{env: env, d: d}
}

func userInfoToProto(u *datastore.UserInfo) *profile.UserInfo {
	return &profile.UserInfo{
		ID:        utils.ProtoFromUUID(&u.ID),
		OrgID:     utils.ProtoFromUUID(&u.OrgID),
		Username:  u.Username,
		FirstName: u.FirstName,
		LastName:  u.LastName,
		Email:     u.Email,
	}
}

func orgInfoToProto(o *datastore.OrgInfo) *profile.OrgInfo {
	return &profile.OrgInfo{
		ID:         utils.ProtoFromUUID(&o.ID),
		OrgName:    o.OrgName,
		DomainName: o.DomainName,
	}
}

func checkValidEmail(email string) error {
	if len(email) == 0 || checkmail.ValidateFormat(email) != nil {
		return errors.New("failed validation")
	}

	components := strings.Split(email, "@")
	if len(components) != 2 {
		return errors.New("malformed email")
	}
	_, domain := components[0], components[1]

	if _, exists := emailDomainBlacklist[domain]; exists {
		return errors.New("disallowed email domain")
	}
	return nil
}

func toExternalError(err error) error {
	if err == datastore.ErrOrgNotFound {
		return status.Error(codes.NotFound, "no such org")
	} else if err == datastore.ErrUserNotFound {
		return status.Error(codes.NotFound, "no such user")
	}
	return err
}

// CreateUser is the GRPC method to create  new user.
func (s *Server) CreateUser(ctx context.Context, req *profile.CreateUserRequest) (*uuidpb.UUID, error) {
	userInfo := &datastore.UserInfo{
		OrgID:     utils.UUIDFromProtoOrNil(req.OrgID),
		Username:  req.Username,
		FirstName: req.FirstName,
		LastName:  req.LastName,
		Email:     req.Email,
	}
	if userInfo.OrgID == uuid.Nil {
		return nil, status.Error(codes.InvalidArgument, "invalid org id")
	}
	if len(userInfo.Username) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid username")
	}
	if len(userInfo.FirstName) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid firstname")
	}
	if len(userInfo.LastName) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid lastname")
	}
	if err := checkValidEmail(userInfo.Email); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	uid, err := s.d.CreateUser(userInfo)
	return utils.ProtoFromUUID(&uid), err
}

// GetUser is the GRPC method to get a user.
func (s *Server) GetUser(ctx context.Context, req *uuidpb.UUID) (*profile.UserInfo, error) {
	uid := utils.UUIDFromProtoOrNil(req)
	userInfo, err := s.d.GetUser(uid)
	if err != nil {
		return nil, err
	}
	if userInfo == nil {
		return nil, status.Error(codes.NotFound, "no such user")
	}
	return userInfoToProto(userInfo), nil
}

// GetUserByEmail is the GRPC method to get a user by email.
func (s *Server) GetUserByEmail(ctx context.Context, req *profile.GetUserByEmailRequest) (*profile.UserInfo, error) {
	userInfo, err := s.d.GetUserByEmail(req.Email)
	if err != nil {
		return nil, toExternalError(err)
	}
	return userInfoToProto(userInfo), nil
}

// CreateOrgAndUser is the GRPC method to create a new org and user.
func (s *Server) CreateOrgAndUser(ctx context.Context, req *profile.CreateOrgAndUserRequest) (*profile.CreateOrgAndUserResponse, error) {
	orgInfo := &datastore.OrgInfo{
		DomainName: req.Org.DomainName,
		OrgName:    req.Org.OrgName,
	}

	userInfo := &datastore.UserInfo{
		Username:  req.User.Username,
		FirstName: req.User.FirstName,
		LastName:  req.User.LastName,
		Email:     req.User.Email,
	}
	if len(orgInfo.DomainName) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid domain name")
	}
	if len(orgInfo.OrgName) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid org name")
	}
	if len(userInfo.Username) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid username")
	}
	if len(userInfo.FirstName) == 0 {
		return nil, status.Error(codes.InvalidArgument, "invalid firstname")
	}
	if err := checkValidEmail(userInfo.Email); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	orgID, userID, err := s.d.CreateUserAndOrg(orgInfo, userInfo)
	if err != nil {
		return nil, err
	}

	md, _ := metadata.FromIncomingContext(ctx)
	ctx = metadata.NewOutgoingContext(ctx, md)

	projectResp, err := s.env.ProjectManagerClient().RegisterProject(ctx, &projectmanagerpb.RegisterProjectRequest{
		OrgID:       utils.ProtoFromUUID(&orgID),
		ProjectName: DefaultProjectName,
	})

	if err != nil {
		deleteErr := s.d.DeleteOrgAndUsers(orgID)
		if deleteErr != nil {
			return nil, status.Error(codes.Internal,
				fmt.Sprintf("Could not delete org and users after create default project failed: %s", err.Error()))
		}
		return nil, err
	}
	if !projectResp.ProjectRegistered {
		return nil, status.Error(codes.Internal, fmt.Sprintf("Could not register project %s", DefaultProjectName))
	}

	resp := &profile.CreateOrgAndUserResponse{
		UserID: utils.ProtoFromUUID(&userID),
		OrgID:  utils.ProtoFromUUID(&orgID),
	}

	return resp, nil
}

// GetOrg is the GRPC method to get an org by ID.
func (s *Server) GetOrg(ctx context.Context, req *uuidpb.UUID) (*profile.OrgInfo, error) {
	orgID := utils.UUIDFromProtoOrNil(req)
	orgInfo, err := s.d.GetOrg(orgID)
	if err != nil {
		return nil, err
	}
	if orgInfo == nil {
		return nil, status.Error(codes.NotFound, "no such org")
	}
	return orgInfoToProto(orgInfo), nil
}

// GetOrgByDomain gets an org by domain name.
func (s *Server) GetOrgByDomain(ctx context.Context, req *profile.GetOrgByDomainRequest) (*profile.OrgInfo, error) {
	orgInfo, err := s.d.GetOrgByDomain(req.DomainName)
	if err != nil {
		return nil, toExternalError(err)
	}
	return orgInfoToProto(orgInfo), nil
}

// DeleteOrgAndUsers deletes an org and all of its users.
func (s *Server) DeleteOrgAndUsers(ctx context.Context, req *uuidpb.UUID) error {
	_, err := s.GetOrg(ctx, req)
	if err != nil {
		return err
	}
	return s.d.DeleteOrgAndUsers(utils.UUIDFromProtoOrNil(req))
}
