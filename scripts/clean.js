const fs = require('fs/promises');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

async function rimraf (filePath) {
  if (!path.isAbsolute(filePath)) {
    filePath = path.resolve(ROOT, filePath);
  }
  await fs.rm(filePath, { recursive: true, force: true });
}


(async () => {
  await rimraf('build');
  await rimraf('api.json');
})();
