import { VizierQueryError } from 'common/errors';
import { Observable, throwError } from 'rxjs';
import { catchError, timeout } from 'rxjs/operators';
import {
    ErrorDetails, ExecuteScriptRequest, HealthCheckRequest, QueryExecutionStats, Relation,
    RowBatchData, Status,
} from 'types/generated/vizier_pb';
import { VizierServiceClient } from 'types/generated/VizierServiceClientPb';
import noop from 'utils/noop';

declare global {
  interface Window {
    __GRPCWEB_DEVTOOLS__: (any) => void;
  }
}

function withDevTools(client) {
  const enableDevTools = window.__GRPCWEB_DEVTOOLS__ || noop;
  enableDevTools([client]);
}

export interface Table {
  relation: Relation;
  data: RowBatchData[];
  name: string;
  id: string;
}

export interface VizierQueryResult {
  queryId?: string;
  tables: Table[];
  status?: Status;
  executionStats?: QueryExecutionStats;
}

export interface VizierQueryArg {
  name: string;
  value?: string;
  variable?: string;
}

export interface VizierQueryFunc {
  name: string;
  outputTablePrefix: string;
  args: VizierQueryArg[];
}

function getExecutionErrors(errList: ErrorDetails[]): string[] {
  return errList.map((error) => {
    switch (error.getErrorCase()) {
      case ErrorDetails.ErrorCase.COMPILER_ERROR: {
        const ce = error.getCompilerError();
        return `Compiler error on line ${ce.getLine()}, column ${ce.getColumn()}: ${ce.getMessage()}.`;
      }
      default:
        return `Unknown error type ${ErrorDetails.ErrorCase[error.getErrorCase()]}.`;
    }
  });
}

const EXECUTE_SCRIPT_TIMEOUT = 5000; // 5 seconds
const HEALTHCHECK_TIMEOUT = 10000; // 10 seconds

export class VizierGRPCClient {
  private client: VizierServiceClient;

  constructor(
    addr: string,
    private token: string,
    private clusterID: string,
    private attachCreds: boolean,
    private vizierVersion: string) {
    this.client = new VizierServiceClient(addr, null, attachCreds ? { withCredentials: 'true' } : {});
    withDevTools(this.client);
  }

  health(): Observable<Status> {
    const headers = {
      ...(this.attachCreds ? {} : { Authorization: `BEARER ${this.token}` }),
    };
    const req = new HealthCheckRequest();
    req.setClusterId(this.clusterID);
    const call = this.client.healthCheck(req, headers);
    return new Observable<Status>((observer) => {
      call.on('data', (resp) => {
        observer.next(resp.getStatus());
      });

      call.on('error', (error) => {
        observer.error(error);
      });

      call.on('end', () => {
        observer.complete();
      });
    }).pipe(
      timeout(HEALTHCHECK_TIMEOUT),
      catchError((err) => {
        call.cancel();
        return throwError(err);
      }),
    );
  }

  // Use a generator to produce the VizierQueryFunc to remove the dependency on vis.tsx.
  // funcsGenerator should correspond to getQueryFuncs in vis.tsx.
  executeScript(script: string, funcs: VizierQueryFunc[]): Promise<VizierQueryResult> {
    const headers = {
      ...(this.attachCreds ? {} : { Authorization: `BEARER ${this.token}` }),
    };
    return new Promise((resolve, reject) => {
      let req: ExecuteScriptRequest;
      try {
        req = this.buildRequest(script, funcs);
      } catch (err) {
        reject(err);
        return;
      }

      const call = this.client.executeScript(req, headers);
      const tablesMap = new Map<string, Table>();
      const results: VizierQueryResult = { tables: [] };
      let resolved = false;

      call.on('data', (resp) => {
        if (!results.queryId) {
          results.queryId = resp.getQueryId();
        }

        if (resp.hasStatus()) {
          const status = resp.getStatus();
          const errList = status.getErrorDetailsList();
          if (errList.length > 0) {
            reject(new VizierQueryError('execution', getExecutionErrors(errList), status));
            return;
          }
          const errMsg = status.getMessage();
          if (errMsg) {
            reject(new VizierQueryError('execution', errMsg, status));
            return;
          }

          results.status = status;
          resolve(results);
          resolved = true;
          return;
        }

        if (resp.hasMetaData()) {
          const relation = resp.getMetaData().getRelation();
          const id = resp.getMetaData().getId();
          const name = resp.getMetaData().getName();
          tablesMap.set(id, { relation, id, name, data: [] });
        } else if (resp.hasData()) {
          const data = resp.getData();
          if (data.hasBatch()) {
            const batch = data.getBatch();
            const id = batch.getTableId();
            const table = tablesMap.get(id);
            if (!table) {
              throw new Error('table does not exisit');
            }
            // Append the data.
            table.data.push(batch);

            // The table is complete.
            if (batch.getEos()) {
              results.tables.push(table);
              tablesMap.delete(id);
              return;
            }
          } else if (data.hasExecutionStats()) {
            // The query finished executing, and all the data has been received.
            results.executionStats = data.getExecutionStats();
            resolve(results);
            resolved = true;
            return;
          }
        }
      });

      call.on('end', () => {
        if (!resolved) {
          reject(new VizierQueryError('execution', 'Execution ended with incomplete results'));
        }
      });

      call.on('error', (err) => {
        reject(new VizierQueryError('server', err.message));
      });

      setTimeout(() => {
        if (!resolved) {
          call.cancel();
          reject(new VizierQueryError('execution', 'Execution timed out'));
        }
      }, EXECUTE_SCRIPT_TIMEOUT);
    });
  }

  private buildRequest(script: string, funcs: VizierQueryFunc[]): ExecuteScriptRequest {
    const req = new ExecuteScriptRequest();
    const errors = [];
    req.setClusterId(this.clusterID);
    req.setQueryStr(script);
    funcs.forEach((input: VizierQueryFunc) => {
      const execFuncPb = new ExecuteScriptRequest.FuncToExecute();
      execFuncPb.setFuncName(input.name);
      execFuncPb.setOutputTablePrefix(input.outputTablePrefix);
      for (const arg of input.args) {
        const argValPb = new ExecuteScriptRequest.FuncToExecute.ArgValue();
        argValPb.setName(arg.name);
        if (typeof arg.value !== 'string') {
          errors.push(`No value provided for arg ${arg.name}.`);
          continue;
        }
        argValPb.setValue(arg.value);
        execFuncPb.addArgValues(argValPb);
      }
      req.addExecFuncs(execFuncPb);
    });

    if (errors.length > 0) {
      throw errors;
    }
    return req;
  }
}
