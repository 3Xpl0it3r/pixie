import { COLOR_SCALE } from 'containers/live/convert-to-vega-spec';
import * as _ from 'lodash';
import { formatFloat64Data } from 'utils/format-data';
import { isArray, isNumber } from 'util';
import { View } from 'vega-typings';

const NUMERAL_FORMAT_STRING = '0.00';

export interface LegendEntry {
  key: string;
  val: string;
  color: string;
}

export interface LegendData {
  time: string;
  entries: LegendEntry[];
}

export const formatLegendData = (view: View, time: number, entries: UnformattedLegendEntry[]): LegendData => {
  const legendData: LegendData = {
    time: new Date(time).toLocaleString(),
    entries: [],
  };
  // Handle the case where `COLOR_SCALE` doesn't exist in vega scales.
  let scale: any;
  try {
    scale = (view as any).scale(COLOR_SCALE);
  } catch (err) {
    // This shouldn't happen but if it does, return empty legend data.
    return legendData;
  }
  legendData.entries = entries.map((entry) => formatLegendEntry(scale, entry.key, entry.val));
  return legendData;
};

const formatLegendEntry = (scale, key: string, val: number): LegendEntry =>  {
  return {
    color: scale(key),
    key,
    val: formatFloat64Data(val, NUMERAL_FORMAT_STRING),
  };
};

interface UnformattedLegendEntry {
  key: string;
  val: number;
}

// TimeHashMap is used to store the entries for the legend for any given time.
// This way we can lookup a time's legend entries in O(1).
interface TimeHashMap {
  [time: number]: UnformattedLegendEntry[];
}

// HoverDataCache is used to cache data about the hover legend. It stores both
// the TimeHashMap, as well as the min and max times of this chart.
// The min/max time are used to ensure the hover line never goes outside the graph.
// This cache is updated whenever vega's data changes.
export interface HoverDataCache {
  timeHashMap: TimeHashMap;
  minTime: number;
  maxTime: number;
}

interface ValidHoverDatum {
  time: number;
  [key: string]: number;
}

const minMaxTimes = (hoverData: ValidHoverDatum[]): {minTime: number, maxTime: number} => {
  const sortedTimes = hoverData.map((datum) => datum.time).sort();
  if (sortedTimes.length === 0) {
    return {minTime: 0, maxTime: Number.MAX_SAFE_INTEGER};
  }
  if (sortedTimes.length < 3) {
    return { minTime: sortedTimes[0], maxTime: sortedTimes[sortedTimes.length - 1]};
  }
  // Because of the time removal hack we have to use the secondmin and second max times, otherwise
  // the hover line will appear off of the graph.
  const secondMinTime = sortedTimes[1];
  let secondMaxTime: number;
  if (sortedTimes.length === 3) {
    // If there are only 3 items, both the secondmin and secondmax will be the middle item.
    secondMaxTime = sortedTimes[1];
  } else {
    secondMaxTime = sortedTimes[sortedTimes.length - 2];
  }
  return {
    minTime: secondMinTime,
    maxTime: secondMaxTime,
  };
};

const keyAvgs = (hoverData: ValidHoverDatum[]): {[key: string]: number} => {
  const keyedAvgState: {[key: string]: {sum: number, n: number}} = {};
  for (const datum of hoverData) {
    for (const [key, val] of Object.entries(datum)) {
      if (key === 'time') { continue; }
      if (!keyedAvgState[key]) {
        keyedAvgState[key] = {sum: 0.0, n: 0};
      }
      keyedAvgState[key].sum += val;
      keyedAvgState[key].n += 1;
    }
  }
  return _.mapValues(keyedAvgState, (state: {sum: number, n: number}) => {
    if (state.n === 0) {
      return 0.0;
    }
    return state.sum / state.n;
  });
};

const buildTimeHashMap = (hoverData: ValidHoverDatum[], sortBy: (key: string) => number): TimeHashMap => {

  const timeHashMap: TimeHashMap = {};
  for (const datum of hoverData) {
    const rest: UnformattedLegendEntry[] = Object.entries(datum).map((entry) => ({key: entry[0], val: entry[1]}))
      .filter((item) => item.key !== 'time' && item.key !== 'sum');
    const sortedRest = _.sortBy(rest, (item) => sortBy(item.key));
    timeHashMap[datum.time] = sortedRest;
  }
  return timeHashMap;
};

export const buildHoverDataCache = (hoverData): HoverDataCache => {
  if (!isArray(hoverData)) {
    return null;
  }
  const validEntries = hoverData.map((entry) => {
    const {time_, ...rest} = entry;
    if (!time_ || !isNumber(time_)) {
      return null;
    }
    const hoverDatum: ValidHoverDatum = {
      time: time_,
    };
    for (const [key, val] of Object.entries(rest)) {
      if (!isNumber(val)) {
        continue;
      }
      hoverDatum[key] = val;
    }
    return hoverDatum;
  }).filter((datum) => datum);

  if (validEntries.length === 0) {
    return;
  }

  const {minTime, maxTime} = minMaxTimes(validEntries);
  const keyedAvgs = keyAvgs(validEntries);

  const timeHashMap: TimeHashMap = buildTimeHashMap(validEntries, (key) => -keyedAvgs[key]);

  return {
    timeHashMap,
    minTime,
    maxTime,
  };
};
