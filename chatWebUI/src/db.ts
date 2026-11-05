import Dexie, { type Table } from 'dexie';

export interface Chat {
    id: string;
    title: string;
    updatedAt: number;
}

export interface Message {
    id?: number;
    chatId: string;
    role: 'user' | 'assistant';
    content: string;
    timestamp: number;
}

export class RavendrelDB extends Dexie {
    chats!: Table<Chat>;
    messages!: Table<Message>;

    constructor() {
        super('RavendrelInferenceDB');
        this.version(1).stores({
            chats: 'id, updatedAt',
            messages: '++id, chatId, timestamp',
        });
    }
}

export const db = new RavendrelDB();
