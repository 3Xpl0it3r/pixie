import { Vis } from 'containers/live/vis';

export interface Arguments {
  [arg: string]: string;
}

export function argsEquals(args1: Arguments, args2: Arguments): boolean {
  if (args1 === args2) {
    return true;
  }

  if (args1 === null || args2 === null) {
    return false;
  }

  if (Object.keys(args1).length !== Object.keys(args2).length) {
    return false;
  }
  const args1Map = new Map(Object.entries(args1));
  for (const [key, val] of Object.entries(args2)) {
    if (args1Map.get(key) !== val) {
      return false;
    }
    args1Map.delete(key);
  }
  if (args1Map.size !== 0) {
    return false;
  }
  return true;
}

// Populate arguments either from defaultValues or from the input args.
export function argsForVis(vis: Vis, args: Arguments, scriptId?: string): Arguments {
  const outArgs: Arguments = {};
  if (!vis) {
    return {};
  }
  if (!args) {
    args = {};
  }
  for (const variable of vis.variables) {
    const val = typeof args[variable.name] !== 'undefined' ? args[variable.name] : variable.defaultValue;
    outArgs[variable.name] = val;
  }
  if (args.script) {
    outArgs.script = args.script;
  }
  if (scriptId) {
    outArgs.script = scriptId;
  }
  return outArgs;
}

// Get the types of the given args, according to the provided vis spec.
export function getArgTypesForVis(vis: Vis): {[arg: string]: string} {
  const types: {[arg: string]: string} = {};

  vis.variables.forEach((v) => {
    types[v.name] = v.type;
  });
  return types;
}
