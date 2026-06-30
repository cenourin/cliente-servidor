# Chat Multicliente em C

Trabalho prático da disciplina de Sistemas Operacionais — IFG Campus Anápolis.

Chat em tempo real com múltiplos clientes via TCP, usando threads POSIX e semáforos para sincronização produtor/consumidor.

## Alunos

- Bruno

## Compilação

```bash
gcc -pthread servidor.c -o servidor
gcc -pthread cliente.c -o cliente
```

## Uso

**Iniciar o servidor:**
```bash
./servidor
```

**Conectar um cliente:**
```bash
./cliente
```

O cliente solicitará o IP do servidor, a porta e o apelido desejado. Para sair, digite `tchau`.

## Protocolo de comunicação

As mensagens seguem o formato:
```
bom|tipo_do_comando|conteudo|eom
```

| Tipo | Direção | Significado |
|---|---|---|
| `msg_servidor` | servidor → clientes | Broadcast de eventos |
| `usuario_entra` | cliente → servidor | Informa o apelido |
| `msg_cliente` | cliente → servidor | Mensagem de chat |
| `usuario_sai` | cliente → servidor | Notifica desconexão |

## Arquitetura

- **Thread principal** — aceita novas conexões (`accept()`)
- **Thread produtora** (1 por cliente) — recebe mensagens e coloca no buffer circular
- **Thread consumidora** (global) — retira do buffer e faz broadcast para todos os clientes
- **Thread auxiliar** (no cliente) — recebe mensagens do servidor em paralelo ao teclado
