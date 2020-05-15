import * as _ from 'lodash';
import * as React from 'react';

import { IconButton } from '@material-ui/core';
import { createStyles, makeStyles, Theme } from '@material-ui/core/styles';
import { CSSProperties } from '@material-ui/core/styles/withStyles';
import KeyboardArrowLeftIcon from '@material-ui/icons/KeyboardArrowLeft';
import KeyboardArrowRightIcon from '@material-ui/icons/KeyboardArrowRight';

import { LegendData, LegendEntry } from './legend-data';

const NUM_ROWS = 2;
const MAX_NUM_GRIDS = 4;

const COLOR_COLUMN_SIZE = 8;
const KEY_COLUMN_SIZE = 160;
const VAL_COLUMN_SIZE = 40;
const COLUMN_GAP_SIZE = 5;
const COLUMN_SIZES = `${COLOR_COLUMN_SIZE}px ${KEY_COLUMN_SIZE}px ${VAL_COLUMN_SIZE}px`;
const GRID_WIDTH = (
  COLOR_COLUMN_SIZE + COLUMN_GAP_SIZE +
  KEY_COLUMN_SIZE + COLUMN_GAP_SIZE +
  VAL_COLUMN_SIZE
);
const GRID_GAP_SIZE = 20;

const calcGridWidth = (numGrids: number) => (numGrids - 1) * GRID_GAP_SIZE + numGrids * GRID_WIDTH;

const ROW_HEIGHT = 25;
const HEADER_HEIGHT = 25;

// I've provided MIN_WIDTH and MIN_HEIGHT here to be used to prevent the user from making too small a chart.
// But I'm not sure how to do that yet.
export const MIN_HEIGHT = HEADER_HEIGHT + (NUM_ROWS * ROW_HEIGHT);
// NUM_GRIDS is scaled up/down based on width, so the minimum witdth is the width of just 1 grid.
export const MIN_WIDTH = GRID_WIDTH;

export interface LegendInteractState {
  selectedSeries: string[];
  hoveredSeries: string;
}

interface LegendProps {
  data: LegendData;
  vegaOrigin: number[];
  chartWidth: number;
  name: string;
  interactState: LegendInteractState;
  setInteractState: (s: LegendInteractState) => void;
}

const useStyles = makeStyles((theme: Theme) => {
  return createStyles({
    gridsContainer: {
      height: '100%',
      width: '100%',
      display: 'flex',
      flexDirection: 'row',
      justifyContent: 'flex-start',
      overflow: 'hidden',
      alignItems: 'center',
    },
    rowContainer: {
      display: 'contents',
    },
    colorCircle: {
      height: `${COLOR_COLUMN_SIZE}px`,
      width: `${COLOR_COLUMN_SIZE}px`,
      borderRadius: '50%',
      marginRight: theme.spacing(1),
      display: 'inline-block',
    },
    colorContainer: {
      textAlign: 'center',
    },
    key: {
      textAlign: 'left',
      textOverflow: 'ellipsis',
      overflow: 'hidden',
      whiteSpace: 'nowrap',
    },
    val: {
      textAlign: 'right',
    },
    gridGap: {
      height: '100%',
      width: `${GRID_GAP_SIZE}px`,
      minWidth: `${GRID_GAP_SIZE}px`,
    },
    iconContainer: {
      display: 'flex',
      flexDirection: 'row',
      justifyContent: 'flex-end',
      flex: '1',
    },
  });
});

const toRowMajorOrder = (entries: LegendEntry[], numCols: number, numRows: number): LegendEntry[] => {
  const newEntries: LegendEntry[] = [];
  outerLoop:
  for (let i = 0; i < numCols; i++) {
    for (let j = 0; j < numRows; j++) {
      const index = j * numCols + i;
      if (index >= entries.length) {
        break outerLoop;
      }
      newEntries.push(entries[index]);
    }
  }
  return newEntries;
};

const Legend = React.memo((props: LegendProps) => {
  const classes = useStyles();
  const [currentPage, setCurrentPage] = React.useState<number>(0);

  const handleRowLeftClick = React.useCallback((key: string, e: React.SyntheticEvent) => {
    // Toggle selected series.
    if (_.includes(props.interactState.selectedSeries, key)) {
      props.setInteractState({
        ...props.interactState,
        selectedSeries: props.interactState.selectedSeries.filter((s: string) => s !== key),
      });
    } else {
      props.setInteractState({
        ...props.interactState,
        selectedSeries: [...props.interactState.selectedSeries, key],
      });
    }
  }, [props.interactState]);

  const handleRowRightClick = React.useCallback((key: string, e: React.SyntheticEvent) => {
    // Reset all selected series.
    props.setInteractState({ ...props.interactState, selectedSeries: [] });
    // Prevent right click menu from showing up.
    e.preventDefault();
    return false;
  }, [props.interactState]);

  const handleRowHover = React.useCallback((key: string, e: React.SyntheticEvent) => {
    props.setInteractState({ ...props.interactState, hoveredSeries: key });
  }, [props.interactState]);

  const handleRowLeave = React.useCallback((e: React.SyntheticEvent) => {
    props.setInteractState({ ...props.interactState, hoveredSeries: null });
  }, [props.interactState]);

  if (props.vegaOrigin.length < 2) {
    return <div />;
  }

  const leftPadding = props.vegaOrigin[0];

  let numGrids = MAX_NUM_GRIDS;
  // Dynamically take out grids if theres no room for them.
  while ((leftPadding + calcGridWidth(numGrids)) > props.chartWidth && numGrids > 1) {
    numGrids--;
  }

  const entriesPerPage = numGrids * NUM_ROWS;
  const maxPages = Math.ceil(props.data.entries.length / entriesPerPage);
  const pageEntriesStart = currentPage * entriesPerPage;
  const pageEntriesEnd = Math.min((currentPage + 1) * entriesPerPage, props.data.entries.length);
  let entries = props.data.entries.slice(pageEntriesStart, pageEntriesEnd);
  entries = toRowMajorOrder(entries, numGrids, NUM_ROWS);

  let index = 0;
  // We add a grid per "column" we want to see in the legend.
  // Each grid contains NUM_ROWS rows and 4 columns: (color circle | key | : | value).
  const grids = [];
  for (let i = 0; i < numGrids; i++) {
    const rows = [];
    // We have to break both here and in the inner loop in case we run out of entries before
    // we have reached all of the grids/rows respectively.
    if (index >= entries.length) {
      break;
    }
    for (let j = 0; j < NUM_ROWS; j++) {
      if (index >= entries.length) {
        break;
      }
      const entry = entries[index];
      index++;
      if (!entry) {
        continue;
      }

      const colorStyles: CSSProperties = {
        backgroundColor: entry.color,
      };

      // Handle hover/selection styling.
      const onMouseOver = (e) => handleRowHover(entry.key, e);
      const styles: CSSProperties = {
        opacity: '1.0',
      };
      if (props.interactState.selectedSeries.length > 0 && !_.includes(props.interactState.selectedSeries, entry.key)) {
        styles.opacity = '0.3';
      }
      if (props.interactState.hoveredSeries === entry.key) {
        styles.color = entry.color;
        styles.opacity = '1.0';
      }

      rows.push(
        <div
          className={classes.rowContainer}
          onMouseOver={onMouseOver}
          onMouseOut={handleRowLeave}
          onClick={(e) => handleRowLeftClick(entry.key, e)}
          onContextMenu={(e) => handleRowRightClick(entry.key, e)}
        >
          <div style={styles} className={classes.colorContainer}>
            <div className={classes.colorCircle} style={colorStyles} />
          </div>
          <div style={styles} className={classes.key}>{entry.key}</div>
          <div style={styles} className={classes.val}>{entry.val}</div>
        </div>);
    }

    const gridStyle: CSSProperties = {
      display: 'grid',
      gridTemplateColumns: COLUMN_SIZES,
      columnGap: `${COLUMN_GAP_SIZE}px`,
      gridTemplateRows: `repeat(${NUM_ROWS}, ${ROW_HEIGHT}px)`,
    };
    const grid = (
      <div style={gridStyle}>
        {rows}
      </div>
    );
    grids.push(grid);
    // If this isn't the last grid, add a spacing div.
    if (i !== numGrids - 1) {
      grids.push(<div className={classes.gridGap}></div>);
    }
  }

  const containerStyles: CSSProperties = {
    paddingLeft: `${leftPadding}px`,
  };

  const handlePageBack = (e) => {
    setCurrentPage((page) => page - 1);
  };

  const handlePageForward = (e) => {
    setCurrentPage((page) => page + 1);
  };

  return (
    <div className={classes.gridsContainer} style={containerStyles}>
      {grids}
      <div className={classes.iconContainer}>
        <IconButton
          onClick={handlePageBack}
          disabled={currentPage === 0}
          size='small'>
          <KeyboardArrowLeftIcon />
        </IconButton>
        <IconButton
          onClick={handlePageForward}
          disabled={currentPage === maxPages - 1}
          size='small'>
          <KeyboardArrowRightIcon />
        </IconButton>
      </div>
    </div>
  );
});
Legend.displayName = 'Legend';

export default Legend;
