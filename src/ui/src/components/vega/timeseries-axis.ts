declare module 'vega-scale' {
  export function scale(type: string, scale?: any, metadata?: any);
  export function tickValues(scale: any, count: number);
}
import { timeFormat } from 'vega-functions';
import { scale, tickValues } from 'vega-scale';

import { textMetrics } from 'vega-scenegraph';

interface TextConfig {
  font: string;
  fontSize: number;
}

interface Label {
  tick: Date;
  label: string;
  formatter?: LabelsFormatter;
  pos: number;
}

interface LabelsFormatter {
  find: (labels: Label[]) => Label[];
  format: (label: Label) => string;
}

// Calculates overlap of left (l) and right (r) components in 1D land.
function overlap(l: Label, r: Label, overlapBuffer: number, textConfig: TextConfig,
                 align: string = 'center'): boolean {
  if (align !== 'center') {
    throw TypeError('only "center" align supported');
  }
  const lWidth = textMetrics.width(textConfig, l.label);
  const rWidth = textMetrics.width(textConfig, r.label);
  return l.pos + lWidth / 2 + overlapBuffer > r.pos - rWidth / 2;
}

function hasOverlap(textConfig: TextConfig, labels: Label[], overlapBuffer: number): boolean {
  for (let i = 1, n = labels.length, a = labels[0], b; i < n; a = b, ++i) {
    if (overlap(a, b = labels[i], overlapBuffer, textConfig)) {
      return true;
    }
  }
  return false;
}

function applyFormat(formatter: LabelsFormatter, labels: Label[]) {
  formatter.find(labels).forEach((l) => {
    l.label = formatter.format(l);
    l.formatter = formatter;
  });
}

// Filters our every other label for overlap testing.
// This follows the default method used in
// `vega-view-transforms/src/Overlap.js`, but adds extra
// functionality to make sure special formatting of labels
// is preserved.
function parityReduce(labels: Label[]): Label[] {
  const specialFormatters = [];
  const newLabels = labels.filter((l, i) => {
    // Keep every other entry.
    if (i % 2 === 0) {
      return true;
    }
    l.label = '';
    // If this label-to-be-removed was formatted, we want re-run the formatter to make sure
    // that the formatting options still apply.
    if (l.formatter) {
      specialFormatters.push(l.formatter);
    }
    return false;
  });
  // Apply any special formatters that were found.
  specialFormatters.forEach((formatterFn) => { applyFormat(formatterFn, newLabels); });
  return newLabels;
}

// Formats the tick value into a time string given the options passed in.
export function formatTime(tick: Date, showAmPm: boolean = false,
                           showDate: boolean = false): string {
  let fmtStr = '%-I:%M:%S';
  if (showAmPm) {
    fmtStr += ' %p';
    if (showDate) {
      fmtStr = '%b %d, %Y ' + fmtStr;
    }
  }
  return timeFormat(tick, fmtStr);
}

function AmPmFormatter(): LabelsFormatter {
  // Add AM and PM to the first and last labels.
  return {
    format: (label: Label): string => {
      return formatTime(label.tick, true);
    },
    find: (labels: Label[]): Label[] => {
      return [labels[0], labels[labels.length - 1]];
    },
  };
}

export function prepareLabels(domain: any, width: number, numTicks: number, overlapBuffer: number,
                              font: string, fontSize: number): Label[] {
  // Gets the true tick values that will be generated.
  const s = scale('time')();
  s.domain(domain);
  const ticks = tickValues(s, numTicks);

  // The config used to measure words.
  const textConfig: TextConfig = {
    font,
    fontSize,
  };

  // Label ticks will be evenly distributed. When alignment is centered
  const labelSep = width / ticks.length;
  const labelStart = 0;

  const labels = [];
  ticks.forEach((tick, i) => {
    labels.push({
      label: formatTime(tick),
      pos: labelStart + labelSep * i,
      tick,
    });
  });

  // Add AM/PM to the first and last labels.
  applyFormat(AmPmFormatter(), labels);

  // We loop through the labels and "hide" (set to empty string) every other
  // label until we no longer have overlapping labels or we only have 3 elements showing.
  let items = labels;
  if (items.length >= 3 && hasOverlap(textConfig, items, overlapBuffer)) {
    do {
      items = parityReduce(items);
    } while (items.length >= 3 && hasOverlap(textConfig, items, overlapBuffer));
  }

  return labels;
}

// This adds the pxTimeFormat function to the passed in vega Module.
export function addPxTimeFormatExpression(vega) {
  if (vega && vega.expressionFunction) {
    const domainFn = vega.expressionFunction('domain');

    let currentWidth = 0;
    let labels = [];

    // Function call by labelExpr in the vega-lite config.
    function pxTimeFormat(datum, width, numTicks, separation, fontName, fontSize) {
      if (datum.index === 0) {
        // Generate the labels on the first run of this function.
        // Subsequent calls of pxTimeFormat will use these results.
        labels =
          prepareLabels(domainFn('x', this), width, numTicks, separation, fontName, fontSize);
        currentWidth = width;
      // } else if (currentWidth !== width) {
        // TODO(philkuz) how should we warn when this happens?
        // console.warn('widths different', 'width', width, 'currentWidth', currentWidth);
      }

      // The denominator of the index.
      const indexDenom = labels.length - 1;

      // TrueIndex is the original index value.
      const trueIndex = Math.abs(Math.round(datum.index * indexDenom));
      // Backup if the trueIndex falls outside of the labels range.
      if (trueIndex >= labels.length) {
        // TODO(philkuz) how should we warn when this happens?
        // console.warn('trueIndex out of range, returning default format for datum. trueIndex:',
        //   datum.index * indexDenom);
        return formatTime(datum.value);
      }
      // Return the label we pre-calculated.
      return labels[trueIndex].label;
    }
    vega.expressionFunction('pxTimeFormat', pxTimeFormat);
  }
}
