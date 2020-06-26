import * as React from 'react';
import { configure, GlobalHotKeys } from 'react-hotkeys';
import { isMac } from 'utils/detect-os';

import Card from '@material-ui/core/Card';
import Modal from '@material-ui/core/Modal';
import { createStyles, makeStyles, Theme } from '@material-ui/core/styles';

type HotKeyAction =
  'pixie-command' |
  'show-help' |
  'toggle-editor' |
  'toggle-data-drawer' |
  'execute';

type KeyMap = {
  [action in HotKeyAction]: {
    sequence: string | string[];
    displaySequence: string | string[];
    description: string;
  }
};

export type Handlers = Omit<{
  [action in HotKeyAction]: () => void;
}, 'show-help'>;

interface LiveViewShortcutsProps {
  handlers: Handlers;
}

export function getKeyMap(): KeyMap {
  const seqPrefix = isMac() ? 'Meta' : 'Control';
  const displayPrefix = isMac() ? 'Cmd' : 'Ctrl';
  const withPrefix = (key: string) => ({
    sequence: `${seqPrefix}+${key}`,
    displaySequence: [displayPrefix, key],
  });
  return {
    'pixie-command': {
      ...withPrefix('k'),
      description: 'Activate Pixie Command',
    },
    'toggle-editor': {
      ...withPrefix('e'),
      description: 'Show/hide script editor',
    },
    'toggle-data-drawer': {
      ...withPrefix('d'),
      description: 'Show/hide data drawer',
    },
    execute: {
      ...withPrefix('enter'),
      description: 'Execute current Live View script',
    },
    'show-help': {
      sequence: 'shift+?', // For some reason just '?' doesn't work.
      displaySequence: '?',
      description: 'Show all keyboard shortcuts',
    },
  };
}

/**
 * Keyboard shortcuts declarations for the live view.
 *
 * The keybindings are declared here, handlers can be registered by child components of the live view.
 */
const LiveViewShortcuts = (props: LiveViewShortcutsProps) => {
  // Run this setup once.
  React.useEffect(() => {
    configure({
      // React hotkeys defaults to ignore events from within ['input', 'select', 'textarea'].
      // We want the Pixie command to work from anywhere.
      ignoreTags: ['select'],
    });
  }, []);

  const [openHelp, setOpenHelp] = React.useState(false);
  const toggleOpenHelp = React.useCallback(() => setOpenHelp((cur) => !cur), []);

  const keyMap: KeyMap = React.useMemo(getKeyMap, []);
  const actionSequences = React.useMemo(() => {
    const map = {};
    for (const key of Object.keys(keyMap)) {
      map[key] = keyMap[key].sequence;
    }
    return map;
  }, []);

  const handlers = React.useMemo(() => {
    const handlerWrapper = (handler) => (e) => {
      e.preventDefault();
      handler();
    };
    const wrappedHandlers = {
      'show-help': toggleOpenHelp,
    };
    for (const action of Object.keys(props.handlers)) {
      wrappedHandlers[action] = handlerWrapper(props.handlers[action]);
    }
    return wrappedHandlers;
  }, [props.handlers]);

  return (
    <>
      <GlobalHotKeys keyMap={actionSequences} handlers={handlers} allowChanges={true} />
      <LiveViewShortcutsHelp keyMap={keyMap} open={openHelp} onClose={toggleOpenHelp} />
    </>
  );
};

export default LiveViewShortcuts;

interface LiveViewShortcutsHelpProps {
  open: boolean;
  onClose: () => void;
  keyMap: KeyMap;
}

const useShortcutHelpStyles = makeStyles((theme: Theme) => createStyles({
  root: {
    width: '500px',
    position: 'absolute',
    top: '50%',
    left: '50%',
    transform: 'translate(-50%, -50%)',
  },
  title: {
    ...theme.typography.subtitle2,
    padding: theme.spacing(2),
  },
  key: {
    border: `solid 2px ${theme.palette.background.three}`,
    borderRadius: '5px',
    backgroundColor: theme.palette.background.two,
    textTransform: 'capitalize',
    height: theme.spacing(4),
    minWidth: theme.spacing(4),
    paddingLeft: theme.spacing(1),
    paddingRight: theme.spacing(1),
    textAlign: 'center',
    ...theme.typography.caption,
    lineHeight: '30px',
  },
  row: {
    display: 'flex',
    flexDirection: 'row',
    borderBottom: `solid 1px ${theme.palette.background.three}`,
    alignItems: 'center',
  },
  sequence: {
    display: 'flex',
    flexDirection: 'row',
    alignItems: 'center',
    flex: 1,
    justifyContent: 'center',
    padding: theme.spacing(1.5),
  },
  description: {
    flex: 3,
  },
}));

const LiveViewShortcutsHelp = (props: LiveViewShortcutsHelpProps) => {
  const classes = useShortcutHelpStyles();
  const { open, onClose, keyMap } = props;
  const makeKey = (key) => <div className={classes.key} key={key}>{key}</div>;

  const shortcuts = Object.keys(keyMap).map((action) => {
    const shortcut = keyMap[action];
    let sequence: React.ReactNode;
    if (Array.isArray(shortcut.displaySequence)) {
      const keys = [];
      for (const key of shortcut.displaySequence) {
        keys.push(makeKey(key));
        keys.push('+');
      }
      keys.pop();
      sequence = keys;
    } else {
      sequence = makeKey(shortcut.displaySequence);
    }
    return (
      <div className={classes.row} key={action}>
        <div className={classes.sequence}>
          {sequence}
        </div>
        <div className={classes.description}>{shortcut.description}</div>
      </div>
    );
  });

  return (
    <Modal open={open} onClose={onClose} BackdropProps={{}}>
      <Card className={classes.root}>
        <div className={classes.title}>
          Available Shortcuts
        </div>
        {shortcuts}
      </Card>
    </Modal>
  );
};
