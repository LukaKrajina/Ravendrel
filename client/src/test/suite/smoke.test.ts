import * as assert from 'assert';
import * as vscode from 'vscode';

suite('Ravendrel Extension Smoke Test', () => {
    test('extension activates and registers runScript command', async () => {
        const commands = await vscode.commands.getCommands(true);
        assert.ok(
            commands.includes('ravendrel.runScript'),
            'ravendrel.runScript command should be registered after activation'
        );
    });
});