/* eslint-disable no-underscore-dangle */
import Legend, { LegendInteractState } from 'components/legend/legend';
import {
  buildHoverDataCache, formatLegendData, HoverDataCache, LegendData,
} from 'components/legend/legend-data';
import {
  ChartDisplay, convertWidgetDisplayToVegaSpec, EXTERNAL_HOVER_SIGNAL, EXTERNAL_TS_DOMAIN_SIGNAL,
  HOVER_PIVOT_TRANSFORM, HOVER_SIGNAL, INTERNAL_HOVER_SIGNAL, INTERNAL_TS_DOMAIN_SIGNAL,
  LEGEND_HOVER_SIGNAL, LEGEND_SELECT_SIGNAL, REVERSE_HOVER_SIGNAL, REVERSE_SELECT_SIGNAL,
  REVERSE_UNSELECT_SIGNAL,
} from 'containers/live/convert-to-vega-spec';
import * as _ from 'lodash';
import * as React from 'react';
import { Vega as ReactVega } from 'react-vega';
import noop from 'utils/noop';
import { View } from 'vega-typings';

import {
  createStyles, makeStyles, useTheme, Theme,
} from '@material-ui/core/styles';

import { TimeSeriesContext } from '../context/time-series-context';

const useStyles = makeStyles((theme: Theme) => createStyles({
  flexbox: {
    height: '100%',
    width: '100%',
    display: 'flex',
    flexDirection: 'column',
  },
  reactVega: {
    flex: 1,
    minHeight: 0,
  },
  legends: {
    height: '15%',
  },
  noDataMsgContainer: {
    height: '100%',
    display: 'flex',
    justifyContent: 'center',
    justifyItems: 'center',
    alignItems: 'center',
  },
  noDataMessage: {
    ...theme.typography.body1,
  },
}));

interface VegaProps {
  data: Array<{}>;
  tableName: string;
  display: ChartDisplay;
  className?: string;
}

const Vega = React.memo((props: VegaProps) => {
  const classes = useStyles();
  const theme = useTheme();
  const { data: inputData, tableName, display } = props;
  const {
    spec, hasLegend, legendColumnName, error,
  } = React.useMemo(() => convertWidgetDisplayToVegaSpec(display, tableName, theme),
    [display, tableName, theme]);

  const data = React.useMemo(() => ({ [tableName]: inputData }), [tableName, inputData]);

  const {
    setHoverTime,
    setTimeseriesDomain: setTSDomain,
    hoverTime: externalHoverTime,
    timeseriesDomain: externalTSDomain,
  } = React.useContext(TimeSeriesContext);

  const [currentView, setCurrentView] = React.useState<View>(null);
  const [vegaOrigin, setVegaOrigin] = React.useState<number[]>([]);
  const [legendData, setLegendData] = React.useState<LegendData>({ time: '', entries: [] });
  const [legendInteractState, setLegendInteractState] = React.useState<LegendInteractState>(
    { selectedSeries: [], hoveredSeries: '' },
  );
  const [hoverDataCache, setHoverDataCache] = React.useState<HoverDataCache>(null);

  const chartRef = React.useRef(null);

  const widthListener = React.useCallback(() => {
    if (!currentView) {
      return;
    }
    setVegaOrigin(currentView.origin());
  }, [currentView]);

  const hoverListener = React.useCallback((name, value) => {
    if (!currentView || !hoverDataCache) {
      return;
    }
    if (value && value.time_) {
      const unformattedEntries = hoverDataCache.timeHashMap[value.time_];
      if (unformattedEntries) {
        setLegendData(formatLegendData(currentView, value.time_, unformattedEntries));
      }
    }
  }, [currentView, hoverDataCache]);

  const updateView = React.useMemo(() => {
    let called = false;
    return () => {
      if (called) {
        return;
      }
      called = true;
      setTimeout(() => {
        currentView.runAsync();
        called = false;
      });
    };
  }, [currentView]);

  const signalListeners = React.useMemo(() => {
    if (!currentView) {
      return {};
    }
    return {
      // Add listener for internal tooltip signal.
      // This internal signal is null, unless the chart is active, so this listener only updates the global
      // hover time context, if this is the active chart.
      [INTERNAL_HOVER_SIGNAL]: (name, value) => {
        if (value && value.time_) {
          setHoverTime(value.time_);
        }
      },

      [INTERNAL_TS_DOMAIN_SIGNAL]: (name, value) => {
        if (!value || value.length !== 2) {
          return;
        }
        const newDomain: [number, number] = [value[0].getTime(), value[1].getTime()];
        setTSDomain((oldDomain) => {
          if (!oldDomain || oldDomain.length !== 2) {
            return newDomain;
          }
          const merged: [number, number] = [Math.min(oldDomain[0], newDomain[0]), Math.max(oldDomain[1], newDomain[1])];
          if (merged[0] === oldDomain[0] && merged[1] === oldDomain[1]) {
            return oldDomain;
          }
          return merged;
        });
      },

      // Add signal listener for width, because the origin changes when width changes.
      width: widthListener,
      // Add signal listener for the merged hover signal. This listener updates the values in the legend.
      [HOVER_SIGNAL]: hoverListener,
      [REVERSE_HOVER_SIGNAL]: (name, value) => {
        if (!value) {
          setLegendInteractState((state) => ({ ...state, hoveredSeries: '' }));
          return;
        }
        setLegendInteractState((state) => ({
          ...state,
          hoveredSeries: value,
        }));
      },
      [REVERSE_SELECT_SIGNAL]: (name, value) => {
        if (!value) {
          return;
        }
        setLegendInteractState((state) => {
          if (_.includes(state.selectedSeries, value)) {
            return { ...state, selectedSeries: state.selectedSeries.filter((s) => s !== value) };
          }
          return { ...state, selectedSeries: [...state.selectedSeries, value] };
        });
      },
      [REVERSE_UNSELECT_SIGNAL]: (name, value) => {
        if (!value) {
          return;
        }
        setLegendInteractState((state) => ({ ...state, selectedSeries: [] }));
      },
    };
  }, [currentView, hoverListener, setHoverTime, setLegendInteractState]);

  const onNewView = React.useCallback((view: View) => {
    // Disable default tooltip handling in vega.
    view.tooltip(noop);

    // Add listener for changes to the legend data. If the data changes we rebuild our hash map cache of the data.
    view.addDataListener(HOVER_PIVOT_TRANSFORM, (name, value) => {
      if (value) {
        const cache = buildHoverDataCache(value);
        if (cache) {
          setHoverDataCache(cache);
        }
      }
    });
    // Width listener only kicks in on width changes, so we have to update on new view as well.
    setVegaOrigin(view.origin());
    setCurrentView(view);
  }, []);

  // This effect sets the hover initial state to be the last time value.
  React.useEffect(() => {
    if (currentView && hoverDataCache && hoverDataCache.maxTime) {
      setHoverTime(hoverDataCache.maxTime);
      // set the legend data since the signal listener might not be added yet when we set the hover signal.
      const unformattedEntries = hoverDataCache.timeHashMap[hoverDataCache.maxTime];
      setLegendData(formatLegendData(currentView, hoverDataCache.maxTime, unformattedEntries));
    }
  }, [hoverDataCache, currentView]);

  React.useEffect(() => {
    if (!currentView) {
      return;
    }
    if (externalTSDomain && externalTSDomain.length === 2
      && !Number.isNaN(externalTSDomain[0]) && !Number.isNaN(externalTSDomain[1])) {
      currentView.signal(EXTERNAL_TS_DOMAIN_SIGNAL, externalTSDomain);
    }
    updateView();
  }, [externalTSDomain, currentView]);

  // Inject the selected series into the corresponding vega signal for this chart.
  React.useEffect(() => {
    if (!currentView || !externalHoverTime || !hoverDataCache) {
      return;
    }
    let time = externalHoverTime;
    // Handle cases where externalHoverTime is not contained within the limits of this chart.
    if (externalHoverTime < hoverDataCache.minTime) {
      time = hoverDataCache.minTime;
    } else if (externalHoverTime > hoverDataCache.maxTime) {
      time = hoverDataCache.maxTime;
    }
    currentView.signal(EXTERNAL_HOVER_SIGNAL, { time_: time });
    updateView();
  }, [externalHoverTime, currentView, hoverDataCache]);

  React.useEffect(() => {
    if (!currentView) {
      return;
    }
    currentView.signal(LEGEND_HOVER_SIGNAL, legendInteractState.hoveredSeries);
    currentView.signal(LEGEND_SELECT_SIGNAL, legendInteractState.selectedSeries);
    updateView();
  }, [currentView, legendInteractState.hoveredSeries, legendInteractState.selectedSeries.length]);

  if (inputData.length === 0) {
    return (
      <div className={props.className}>
        <div className={classes.noDataMsgContainer}>
          <span className={classes.noDataMessage}>{`No data available for ${tableName} table`}</span>
        </div>
      </div>
    );
  }

  return (
    <div className={props.className}>
      {error ? <div>{error.toString()}</div>
        : (
          <div className={classes.flexbox} ref={chartRef}>
            <ReactVega
              spec={spec}
              data={data}
              actions={false}
              onNewView={onNewView}
              className={classes.reactVega}
              signalListeners={signalListeners}
            />
            {!hasLegend ? null
              : (
                <div className={classes.legends}>
                  <Legend
                    data={legendData}
                    vegaOrigin={vegaOrigin}
                    chartWidth={chartRef.current ? chartRef.current.getBoundingClientRect().width : 0}
                    name={legendColumnName}
                    interactState={legendInteractState}
                    setInteractState={setLegendInteractState}
                  />
                </div>
              )}
          </div>
        )}
    </div>
  );
});
export default Vega;
