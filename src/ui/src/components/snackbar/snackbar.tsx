import * as React from 'react';
import noop from 'utils/noop';

import {createStyles, makeStyles, Theme} from '@material-ui/core';
import Button from '@material-ui/core/Button';
import IconButton from '@material-ui/core/IconButton';
import Snackbar from '@material-ui/core/Snackbar';
import CloseIcon from '@material-ui/icons/Close';

interface ShowArgs {
  message: string;
  action?: () => void;
  actionTitle?: string;
  autoHideDuration?: number;
  dismissible?: boolean;
}

type ShowSnackbarFunc = (args: ShowArgs) => void;

const SnackbarContext = React.createContext<ShowSnackbarFunc>(null);

type SnackbarState = {
  opened: boolean;
} & Required<ShowArgs>;

const useStyles = makeStyles((theme: Theme) =>
  createStyles({
    snackbar: {
      backgroundColor: theme.palette.background.three,
      color: theme.palette.text.secondary,
    },
  }),
);

export const SnackbarProvider: React.FC = (props) => {
  const classes = useStyles();
  const [state, setState] = React.useState<SnackbarState>({
    opened: false,
    message: '',
    action: noop,
    actionTitle: '',
    autoHideDuration: 2000,
    dismissible: true,
  });

  const showSnackbar = React.useCallback((args: ShowArgs) => {
    const {
      message,
      action = noop,
      actionTitle = '',
      autoHideDuration = 2000,
      dismissible = true,
    } = args;
    setState({
      message,
      action,
      actionTitle,
      autoHideDuration,
      dismissible,
      opened: true,
    });
  }, []);

  const hideSnackbar = React.useCallback(() => {
    setState((prevState) => ({
      ...prevState,
      opened: false,
    }));
  }, []);

  const snackbarAction = React.useMemo(() => (
    <>
      {
        state.action !== noop && state.actionTitle && (
          <Button
            onClick={() => {
              state.action();
              hideSnackbar();
            }}
            color='primary'
          >
            {state.actionTitle}
          </Button>
        )
      }
      {
        state.dismissible && (
          <IconButton onClick={hideSnackbar} color='inherit'>
            <CloseIcon />
          </IconButton>
        )
      }
    </>
  ), [state.dismissible, state.action, state.actionTitle]);
  return (
    <>
      <SnackbarContext.Provider value={showSnackbar}>
        {props.children}
      </SnackbarContext.Provider>
      <Snackbar
        ContentProps={{ className: classes.snackbar }}
        open={state.opened}
        onClose={hideSnackbar}
        message={state.message}
        action={snackbarAction}
        autoHideDuration={state.autoHideDuration}
      />
    </>
  );
};

export function useSnackbar(): ShowSnackbarFunc {
  const show = React.useContext(SnackbarContext);
  if (!show) {
    throw new Error('useSnackbar must be call from within SnackbarProvider');
  }
  return show;
}
