package datastore

import (
	"database/sql"

	"github.com/jmoiron/sqlx"
	uuid "github.com/satori/go.uuid"
)

// Datastore implementation for projectmanager using a PGSQL backend.
type Datastore struct {
	db *sqlx.DB
}

// ProjectInfo describes a specific project.
type ProjectInfo struct {
	OrgID       uuid.UUID `db:"org_id"`
	ProjectName string    `db:"project_name"`
}

// NewDatastore creates a new project manager Datastore.
func NewDatastore(db *sqlx.DB) (*Datastore, error) {
	return &Datastore{db: db}, nil
}

// CheckAvailability checks the database to see if the project is still available.
func (d *Datastore) CheckAvailability(orgID uuid.UUID, projectName string) (bool, error) {
	var exists bool

	query := `SELECT exists(SELECT project_name from projects WHERE project_name=$1 and org_id=$2)`
	err := d.db.QueryRow(query, projectName, orgID).Scan(&exists)
	if err != nil && err != sql.ErrNoRows {
		return false, err
	}

	return !exists, nil
}

// RegisterProject assigs ownership of the project to a particular org name. Doing this on a project that already exists is an error.
func (d *Datastore) RegisterProject(orgID uuid.UUID, projectName string) error {
	query := `INSERT INTO projects (org_id, project_name) VALUES ($1, $2)`
	_, err := d.db.Exec(query, orgID, projectName)
	return err
}

// GetProjectForOrg gets the project for a particular org specified by ID.
func (d *Datastore) GetProjectForOrg(orgID uuid.UUID) (*ProjectInfo, error) {
	query := `SELECT org_id, project_name from projects WHERE org_id=$1`
	projectInfo := &ProjectInfo{}
	row := d.db.QueryRowx(query, orgID)
	switch err := row.StructScan(projectInfo); err {
	case sql.ErrNoRows:
		return nil, nil
	case nil:
		return projectInfo, nil
	default:
		return nil, err
	}
}

// GetProjectByName gets the project based on the project name.
func (d *Datastore) GetProjectByName(orgID uuid.UUID, projectName string) (*ProjectInfo, error) {
	query := `SELECT org_id, project_name from projects WHERE project_name=$1 and org_id=$2`
	projectInfo := &ProjectInfo{}
	row := d.db.QueryRowx(query, projectName, orgID)
	switch err := row.StructScan(projectInfo); err {
	case sql.ErrNoRows:
		return nil, nil
	case nil:
		return projectInfo, nil
	default:
		return nil, err
	}
}
