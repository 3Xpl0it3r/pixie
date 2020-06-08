import { scrollbarStyles } from 'common/mui-theme';
import ClientContext, { VizierGRPCClientProvider } from 'common/vizier-grpc-client-context';
import ProfileMenu from 'containers/live/profile-menu';
import { distanceInWords } from 'date-fns';
import gql from 'graphql-tag';
import * as React from 'react';
import { Link, useParams } from 'react-router-dom';
import { dataFromProto } from 'utils/result-data-utils';

import { useQuery } from '@apollo/react-hooks';
import Breadcrumbs from '@material-ui/core/Breadcrumbs';
import { createStyles, makeStyles, Theme, withStyles } from '@material-ui/core/styles';
import Tab from '@material-ui/core/Tab';
import Table from '@material-ui/core/Table';
import TableBody from '@material-ui/core/TableBody';
import TableHead from '@material-ui/core/TableHead';
import TableRow from '@material-ui/core/TableRow';
import Tabs from '@material-ui/core/Tabs';
import Tooltip from '@material-ui/core/Tooltip';
import Typography from '@material-ui/core/Typography';

import {
    AdminTooltip, convertHeartbeatMS, StatusCell, StyledLeftTableCell, StyledRightTableCell,
    StyledTab, StyledTableCell, StyledTableHeaderCell, StyledTabs, VizierStatusGroup,
} from './utils';

const useStyles = makeStyles((theme: Theme) => {
  return createStyles({
    root: {
      height: '100%',
      width: '100%',
      display: 'flex',
      flexDirection: 'column',
      backgroundColor: theme.palette.background.default,
      color: theme.palette.text.primary,
      ...scrollbarStyles(theme),
    },
    topBar: {
      display: 'flex',
      margin: theme.spacing(1),
      alignItems: 'center',
    },
    title: {
      flexGrow: 1,
      marginLeft: theme.spacing(2),
    },
    main: {
      flex: 1,
      minHeight: 0,
      borderTopStyle: 'solid',
      borderTopColor: theme.palette.background.three,
      borderTopWidth: theme.spacing(0.25),
    },
    error: {
      padding: 20,
    },
    link: {
      ...theme.typography.subtitle1,
      margin: theme.spacing(1),
    },
    titleText: {
      ...theme.typography.h6,
      fontWeight: theme.typography.fontWeightBold,
    },
    breadcrumbText: {
      ...theme.typography.subtitle2,
      fontWeight: theme.typography.fontWeightLight,
      color: '#748790',
    },
    breadcrumbLink: {
      ...theme.typography.subtitle2,
    },
    tabContents: {
      margin: theme.spacing(2),
    },
  });
});

const AGENT_STATUS_SCRIPT = `import px
px.display(px.GetAgentStatus())`;

const AGENTS_POLL_INTERVAL = 2500;

interface AgentDisplay {
  id: string;
  idShort: string;
  status: string;
  statusGroup: VizierStatusGroup;
  hostname: string;
  lastHeartbeat: string;
  uptime: string;
}

function getAgentStatusGroup(status: string): VizierStatusGroup {
  if (['AGENT_STATE_HEALTHY'].indexOf(status) != -1) {
    return 'healthy';
  } else if (['AGENT_STATE_UNRESPONSIVE'].indexOf(status) != -1) {
    return 'unhealthy';
  } else {
    return 'unknown';
  }
}

export function formatAgent(agentInfo): AgentDisplay {
  const now = new Date();

  return {
    id: agentInfo.agent_id,
    idShort: agentInfo.agent_id.split('-').pop(),
    status: agentInfo.agent_state.replace('AGENT_STATE_', ''),
    statusGroup: getAgentStatusGroup(agentInfo.agent_state),
    hostname: agentInfo.hostname,
    lastHeartbeat: convertHeartbeatMS(agentInfo.last_heartbeat_ns / (1000 * 1000)),
    uptime: distanceInWords(new Date(agentInfo.create_time), now, { addSuffix: false }),
  };
}

const AgentsTableContent = ({ agents }) => {
  const agentsDisplay = agents.map((agent) => formatAgent(agent));
  return (
    <Table>
      <TableHead>
        <TableRow>
          <StyledTableHeaderCell></StyledTableHeaderCell>
          <StyledTableHeaderCell>ID</StyledTableHeaderCell>
          <StyledTableHeaderCell>Hostname</StyledTableHeaderCell>
          <StyledTableHeaderCell>Last Heartbeat</StyledTableHeaderCell>
          <StyledTableHeaderCell>Uptime</StyledTableHeaderCell>
        </TableRow>
      </TableHead>
      <TableBody>
        {agentsDisplay.map((agent) => (
          <TableRow key={agent.id}>
            <AdminTooltip title={agent.status}>
              <StyledLeftTableCell>
                <StatusCell statusGroup={agent.statusGroup} />
              </StyledLeftTableCell>
            </AdminTooltip>
            <AdminTooltip title={agent.id}>
              <StyledTableCell>{agent.idShort}</StyledTableCell>
            </AdminTooltip>
            <StyledTableCell>{agent.hostname}</StyledTableCell>
            <StyledTableCell>{agent.lastHeartbeat}</StyledTableCell>
            <StyledRightTableCell>{agent.uptime}</StyledRightTableCell>
          </TableRow>
        ))}
      </TableBody>
    </Table>
  );
}

interface AgentDisplayState {
  error?: string;
  data: Array<{}>;
}

const AgentsTable = () => {
  const { client } = React.useContext(ClientContext);
  const [state, setState] = React.useState<AgentDisplayState>({ data: [] });

  React.useEffect(() => {
    if (!client) {
      return;
    }
    let mounted = true;
    const fetchAgentStatus = () => {
      client.executeScript(AGENT_STATUS_SCRIPT, []).then((results) => {
        if (!mounted) {
          return;
        }
        if (results.tables.length !== 1) {
          if (results.status) {
            setState({ ...state, error: results.status.getMessage() });
          }
          return;
        }
        const data = dataFromProto(results.tables[0].relation, results.tables[0].data);
        setState({ data });
      }).catch((error) => {
        if (!mounted) {
          return;
        }
        setState({ ...state, error: error?.message });
      });
    };
    fetchAgentStatus();
    const interval = setInterval(fetchAgentStatus, AGENTS_POLL_INTERVAL);
    return () => {
      clearInterval(interval);
      mounted = false;
    };
  }, [client]);

  if (state.error) {
    return <span>Error! {state.error}</span>;
  }
  return <AgentsTableContent agents={state.data} />;
}

const LIST_CLUSTERS = gql`
{
  clusters {
    id
    status
    clusterName
    vizierConfig {
      passthroughEnabled
    }
  }
}
`;

const ClusterDetailsContents = ({name}) => {
  const classes = useStyles();
  const [tab, setTab] = React.useState('agents');

  const { loading, error, data } = useQuery(LIST_CLUSTERS, { pollInterval: AGENTS_POLL_INTERVAL });

  if (loading) {
    return <div className={classes.error}>Loading...</div>;
  }
  if (error) {
    return <div className={classes.error}>{error}</div>;
  }
  if (!data || !data.clusters) {
    return <div className={classes.error}>No clusters found.</div>;
  }

  const cluster = data.clusters.find((c) => c.clusterName === name);
  if (!cluster) {
    return <div className={classes.error}>Cluster {name} not found.</div>;
  }

  return (
    <div>
      <StyledTabs
        value={tab}
        onChange={(event, newTab) => setTab(newTab)}
      >
        <StyledTab value='agents' label='Agents' />
      </StyledTabs>
      {
        tab === 'agents' &&
        (
          <VizierGRPCClientProvider
            clusterID={cluster.id}
            passthroughEnabled={cluster.vizierConfig.passthroughEnabled}
            clusterStatus={cluster.status}
          >
            <AgentsTable />
          </VizierGRPCClientProvider>
        )
      }
    </div>
  );
}

export const ClusterDetailsPage = () => {
  const classes = useStyles();
  const { name } = useParams();
  return (
    <div className={classes.root}>
      <div className={classes.topBar}>
        <div className={classes.title}>
          <div className={classes.titleText}>Cluster View</div>
          <Breadcrumbs classes={{ separator: classes.breadcrumbText }}>
            <Link className={classes.breadcrumbLink} to='/admin'>Admin</Link>
            <Typography className={classes.breadcrumbText}>Cluster</Typography>
            <Typography className={classes.breadcrumbText}>{name}</Typography>
          </Breadcrumbs>
        </div>
        <Link className={classes.link} to='/live'>Live View</Link>
        <ProfileMenu />
      </div>
      <div className={classes.main}>
        <ClusterDetailsContents name={name}/>
      </div>
    </div>
  );
}
