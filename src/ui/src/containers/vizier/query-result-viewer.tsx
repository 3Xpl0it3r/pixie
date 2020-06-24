import './query-result-viewer.scss';

import Tooltip from '@material-ui/core/Tooltip';
import clsx from 'clsx';
import ClusterContext from 'common/cluster-context';
import { Table } from 'common/vizier-grpc-client';
import { StatusCell, StatusGroup } from 'components/status/status';
import {
    AutoSizedScrollableTable, AutoSizedScrollableTableProps, TableColumnInfo,
} from 'components/table/scrollable-table';
import { isEntityType, toEntityPathname, toSingleEntityPage } from 'containers/live/utils/live-view-params';
import * as React from 'react';
import { Link } from 'react-router-dom';
import { DataType, Relation, SemanticType} from 'types/generated/vizier_pb';
import * as FormatData from 'utils/format-data';
import { dataFromProto } from 'utils/result-data-utils';

// STATUS_TYPES contains types that should be displayed as a status indicator.
const STATUS_TYPES = new Set<SemanticType>([
  SemanticType.ST_POD_PHASE,
  SemanticType.ST_CONTAINER_STATE,
]);

function computeColumnWidthRatios(relation: Relation, parsedTable: any): any {
  // Compute the average data width of a column (by name).
  const aveColWidth = {};
  let totalWidth = 0;
  relation.getColumnsList().forEach((col) => {
    const colName = col.getColumnName();
    aveColWidth[colName] = parsedTable.reduce((acc, val) => (
      acc + (val[colName].length / parsedTable.length)), 0);
    totalWidth += aveColWidth[colName];
  });

  const colWidthRatio = {};
  relation.getColumnsList().forEach((col) => {
    const colName = col.getColumnName();
    colWidthRatio[colName] = aveColWidth[colName] / totalWidth;
  });

  return colWidthRatio;
}

function toEntityLink(entity: string, semanticType: SemanticType, clusterName: string) {
  const page = toSingleEntityPage(entity, semanticType, clusterName);
  const pathname = toEntityPathname(page);
  return <Link to={pathname} className={'query-results--entity-link'}>{entity}</Link>;
}

function containerStateToStatusGroup(status: string): StatusGroup {
  switch (status) {
    case 'Running':
      return 'healthy';
    case 'Terminated':
      return 'unhealthy';
    case 'Waiting':
      return 'pending';
    case 'Unknown':
    default:
      return 'unknown';
  }
}

function podPhaseToStatusGroup(status: string): StatusGroup {
  switch (status) {
    case 'Running':
    case 'Succeeded':
      return 'healthy';
    case 'Failed':
      return 'unhealthy';
    case 'Pending':
      return 'pending';
    case 'Unknown':
    default:
      return 'unknown';
  }
}

function toStatusIndicator(status: string, semanticType: SemanticType) {
  let statusGroup: StatusGroup;
  if (semanticType === SemanticType.ST_CONTAINER_STATE) {
    statusGroup = containerStateToStatusGroup(status);
  } else if (semanticType === SemanticType.ST_POD_PHASE) {
    statusGroup = podPhaseToStatusGroup(status);
  } else {
    return status;
  }
  return (
    <Tooltip title={status} interactive>
      <div>
        <StatusCell statusGroup={statusGroup}/>
      </div>
    </Tooltip>
  );
}

function ResultCellRenderer(cellData: any, columnInfo: TableColumnInfo) {
  const dataType = columnInfo.dataType;
  const colName = columnInfo.label;

  if (isEntityType(columnInfo.semanticType)) {
    // Hack to handle cases like "['pl/service1', 'pl/service2']" which show up for pods that are part of 2 services.
    if (columnInfo.semanticType === SemanticType.ST_SERVICE_NAME) {
      try {
        const parsedArray = JSON.parse(cellData);
        if (Array.isArray(parsedArray)) {
          return (
            <>
              {
                parsedArray.map((entity, i) => {
                  return (
                    <span key={i}>
                      {i > 0 && ', '}
                      {toEntityLink(entity, columnInfo.semanticType, columnInfo.clusterName)}
                    </span>
                  );
                })
              }
            </>
          );
        }
      } catch (e) {
        //
      }
    }
    return toEntityLink(cellData, columnInfo.semanticType, columnInfo.clusterName);
  }

  if (STATUS_TYPES.has(columnInfo.semanticType)) {
    return toStatusIndicator(cellData, columnInfo.semanticType);
  }

  const data = cellData;

  if (FormatData.looksLikeLatencyCol(colName, dataType)) {
    return FormatData.LatencyData(data);
  }

  if (FormatData.looksLikeAlertCol(colName, dataType)) {
    return FormatData.AlertData(data);
  }

  if (dataType !== DataType.STRING) {
    return data;
  }

  try {
    const jsonObj = JSON.parse(cellData);
    return <FormatData.JSONData
      data={jsonObj}
    />;
  } catch {
    return data;
  }
}

function ExpandedRowRenderer(rowData) {
  return <FormatData.JSONData
    className='query-results-expanded-row'
    data={rowData}
    multiline={true}
  />;
}

function parseTable(table: Table, clusterName: string): AutoSizedScrollableTableProps {
  const parsedTable = dataFromProto(table.relation, table.data);
  const colWidthRatio = computeColumnWidthRatios(table.relation, parsedTable);

  // TODO(zasgar/michelle): Clean this up and make sure it's consistent with the
  // CSS.
  const colWidth = 600;
  const minColWidth = 200;
  const columnInfo: TableColumnInfo[] = table.relation.getColumnsList().map((col) => {
    const colName = col.getColumnName();
    return {
      dataKey: colName,
      label: colName,
      clusterName: clusterName,
      dataType: col.getColumnType(),
      semanticType: col.getColumnSemanticType(),
      flexGrow: 8,
      width: Math.max(minColWidth, colWidthRatio[colName] * colWidth),
    };
  });
  return {
    data: parsedTable,
    columnInfo,
    cellRenderer: ResultCellRenderer,
    expandable: true,
    expandRenderer: ExpandedRowRenderer,
    resizableCols: true,
  };
}

export interface QueryResultTableProps {
  data: Table;
  className?: string;
}

export const QueryResultTable = React.memo<QueryResultTableProps>(({ data, className }) => {
  const { selectedClusterName } = React.useContext(ClusterContext);
  const props = parseTable(data, selectedClusterName);
  return (
    <div className={clsx('query-results', className)}>
      <AutoSizedScrollableTable
        {...props}
      />
    </div>
  );
});

QueryResultTable.displayName = 'QueryResultTable';
