import * as path from 'path';
import { runTests } from '@vscode/test-electron';

// 集成测试入口:下载/启动 VSCode,加载扩展,运行 suite。
// 通过 `npm run test:integration` 运行(首次会下载 VSCode,较重)。
async function main() {
    try {
        const extensionDevelopmentPath = path.resolve(__dirname, '../../../');
        const extensionTestsPath = path.resolve(__dirname, './suite/index');
        await runTests({
            extensionDevelopmentPath,
            extensionTestsPath,
            launchArgs: ['--disable-extensions']
        });
    } catch (err) {
        console.error('Failed to run integration tests:', err);
        process.exit(1);
    }
}

main();
