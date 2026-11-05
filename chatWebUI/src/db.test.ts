import 'fake-indexeddb/auto';
import { describe, it, expect, beforeEach } from 'vitest';
import { db } from './db';

beforeEach(async () => {
    await db.chats.clear();
    await db.messages.clear();
});

describe('RavendrelDB', () => {
    it('persists chats ordered by updatedAt desc', async () => {
        await db.chats.put({ id: 'a', title: 'A', updatedAt: 1 });
        await db.chats.put({ id: 'b', title: 'B', updatedAt: 2 });
        const chats = await db.chats.orderBy('updatedAt').reverse().toArray();
        expect(chats[0].id).toBe('b');
    });

    it('filters messages by chatId', async () => {
        await db.messages.add({ chatId: 'c1', role: 'user', content: 'hi', timestamp: 1 });
        await db.messages.add({ chatId: 'c2', role: 'user', content: 'yo', timestamp: 2 });
        const msgs = await db.messages.where('chatId').equals('c1').toArray();
        expect(msgs).toHaveLength(1);
        expect(msgs[0].content).toBe('hi');
    });
});
