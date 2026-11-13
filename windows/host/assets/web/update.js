(() => {
  const parts = value => {
    if (!/^[vV]?\d+\.\d+\.\d+$/.test(value)) throw new Error('Invalid version');
    return value.replace(/^[vV]/, '').split('.').map(Number);
  };
  const isNewer = (latest, current) => {
    const a = parts(latest), b = parts(current);
    for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
    return false;
  };
  const check = async current => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    try {
      const response = await fetch('https://api.github.com/repos/liujiaxing060903/WireView/releases/latest', {
        headers: { Accept: 'application/vnd.github+json' }, cache: 'no-store', signal: controller.signal
      });
      if (!response.ok) throw new Error('Update check failed');
      const release = await response.json();
      if (release.draft || release.prerelease) throw new Error('Not a stable release');
      return { version: release.tag_name.replace(/^[vV]/, ''), newer: isNewer(release.tag_name, current) };
    } finally {
      clearTimeout(timer);
    }
  };
  window.WireViewUpdate = { isNewer, check };
})();
