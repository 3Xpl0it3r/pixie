import * as React from 'react';

import SvgIcon, { SvgIconProps } from '@material-ui/core/SvgIcon';

const MagicIcon = (props: SvgIconProps) => (
  <SvgIcon {...props} viewBox='0 0 17.91 19.38'>
    <path d='M6 3V5H7V3H9V2H7V0H6V2H4V3H6Z' />
    <path fillRule='evenodd' clipRule='evenodd'
      d={`M17.3807 5.41424L14.5522 2.58582L0.585449 16.5525L3.41388 19.3809L17.3807 5.41424ZM13.4969
        7.88381L12.0827 6.46959L14.5522 4.00003L15.9664 5.41424L13.4969 7.88381Z`} />
    <path d='M17 13.5V12.5H16V12H17V11H17.5V12H18.5V12.5H17.5V13.5H17Z' />
  </SvgIcon>
);

export default MagicIcon;
