// 同级应用对比：与 cboot 的 BenchApi 做相同的每请求工作
//
// cboot 侧（test/bench.cpp）：
//   /ping -> "pong"
//   /json -> 反射序列化 BenchJson{"hello, world!", 42}
// 本实现：
//   /ping -> "pong"
//   /json -> 每请求构造 {"message":"hello, world!","id":42}
//  其他  -> 404
//
// 响应头逐请求构造（对齐 cboot 的 http_append_response，含 Content-Length /
// Keep-Alive），单线程 Selector，读缓冲复用。用于回答：
//   "同样是每请求真序列化，C++ 与 Java 的每核差距是多少"
// 与 BenchServer.java（预编码响应）的区别：那份是 Java 的零点下限，这份是同级应用。
//
// 用法：java BenchServerFair [port=18081]

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

public class BenchServerFair {
    private static final byte[] PONG = "pong".getBytes(StandardCharsets.UTF_8);
    private static final byte[] NOT_FOUND = "not found".getBytes(StandardCharsets.UTF_8);
    private static final String TYPE_TEXT = "text/plain; charset=utf-8";
    private static final String TYPE_JSON = "application/json";

    private final Selector selector;
    private final Map<SocketChannel, Conn> conns = new HashMap<SocketChannel, Conn>();
    private final ByteBuffer readBuf = ByteBuffer.allocate(8192);

    static final class Conn {
        final SocketChannel ch;
        byte[] in = new byte[8192];
        int inLen;
        byte[] out = new byte[8192];
        int outLen;
        int outPos;

        Conn(SocketChannel ch) { this.ch = ch; }

        void append(byte[] data) {
            if (outPos == outLen) { outPos = 0; outLen = 0; }
            if (outLen + data.length > out.length) {
                int cap = out.length;
                while (cap < outLen + data.length) cap <<= 1;
                byte[] grown = new byte[cap];
                System.arraycopy(out, 0, grown, 0, outLen);
                out = grown;
            }
            System.arraycopy(data, 0, out, outLen, data.length);
            outLen += data.length;
        }

        void consume(int n) {
            System.arraycopy(in, n, in, 0, inLen - n);
            inLen -= n;
        }
    }

    BenchServerFair() throws IOException {
        selector = Selector.open();
    }

    void run(int port) throws IOException {
        ServerSocketChannel ssc = ServerSocketChannel.open();
        ssc.bind(new InetSocketAddress(port), 1024);
        ssc.configureBlocking(false);
        ssc.register(selector, SelectionKey.OP_ACCEPT);
        System.out.println("java fair bench server listening on " + port);
        System.out.flush();

        while (true) {
            selector.select(200);
            Iterator<SelectionKey> it = selector.selectedKeys().iterator();
            while (it.hasNext()) {
                SelectionKey key = it.next();
                it.remove();
                if (key.isAcceptable()) {
                    accept((ServerSocketChannel) key.channel());
                } else if (key.isWritable() || key.isReadable()) {
                    SocketChannel ch = (SocketChannel) key.channel();
                    Conn c = conns.get(ch);
                    if (c == null) continue;
                    try {
                        if (key.isReadable()) onRead(c);
                        if (key.isValid() && key.isWritable()) flush(c);
                    } catch (IOException e) {
                        close(c);
                    }
                }
            }
        }
    }

    void accept(ServerSocketChannel ssc) throws IOException {
        SocketChannel ch;
        while ((ch = ssc.accept()) != null) {
            ch.configureBlocking(false);
            ch.register(selector, SelectionKey.OP_READ);
            conns.put(ch, new Conn(ch));
        }
    }

    void onRead(Conn c) throws IOException {
        readBuf.clear();
        int n = c.ch.read(readBuf);
        if (n < 0) { close(c); return; }
        if (n == 0) return;
        if (c.inLen + n > c.in.length) {
            int cap = c.in.length;
            while (cap < c.inLen + n) cap <<= 1;
            byte[] grown = new byte[cap];
            System.arraycopy(c.in, 0, grown, 0, c.inLen);
            c.in = grown;
        }
        System.arraycopy(readBuf.array(), 0, c.in, c.inLen, n);
        c.inLen += n;

        while (true) {
            int he = indexOfHeaderEnd(c.in, c.inLen);
            if (he < 0) break;
            int sp1 = indexOf(c.in, c.inLen, (byte) ' ', 0, he);
            int sp2 = sp1 < 0 ? -1 : indexOf(c.in, c.inLen, (byte) ' ', sp1 + 1, he);
            if (sp1 < 0 || sp2 < 0) { close(c); return; }
            String target = new String(c.in, sp1 + 1, sp2 - sp1 - 1,
                    StandardCharsets.US_ASCII);
            c.consume(he + 4);

            int q = target.indexOf('?');
            String path = q < 0 ? target : target.substring(0, q);
            if (path.equals("/ping")) {
                appendResponse(c, 200, "OK", TYPE_TEXT, PONG);
            } else if (path.equals("/json")) {
                appendResponse(c, 200, "OK", TYPE_JSON, jsonBody());
            } else {
                appendResponse(c, 404, "Not Found", TYPE_TEXT, NOT_FOUND);
            }
        }
        flush(c);
    }

    /** 每请求真序列化：字段拼接 + 转义 + UTF-8 编码（等价于 cboot 的反射写入） */
    static byte[] jsonBody() {
        StringBuilder sb = new StringBuilder(64);
        sb.append("{\"message\":\"");
        escape(sb, "hello, world!");
        sb.append("\",\"id\":").append(42).append('}');
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    static void escape(StringBuilder sb, String s) {
        for (int i = 0; i < s.length(); ++i) {
            char ch = s.charAt(i);
            switch (ch) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\b': sb.append("\\b");  break;
                case '\f': sb.append("\\f");  break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:
                    if (ch < 0x20) {
                        sb.append(String.format("\\u%04x", (int) ch));
                    } else {
                        sb.append(ch);
                    }
            }
        }
    }

    /** 逐请求构造响应头 + 体（对齐 cboot 的 http_append_response） */
    static void appendResponse(Conn c, int status, String reason, String type, byte[] body) {
        StringBuilder h = new StringBuilder(160);
        h.append("HTTP/1.1 ").append(status).append(' ').append(reason).append("\r\n");
        h.append("Content-Type: ").append(type).append("\r\n");
        h.append("Content-Length: ").append(body.length).append("\r\n");
        h.append("Connection: keep-alive\r\nKeep-Alive: timeout=15\r\n\r\n");
        c.append(h.toString().getBytes(StandardCharsets.US_ASCII));
        c.append(body);
    }

    void flush(Conn c) throws IOException {
        while (c.outPos < c.outLen) {
            ByteBuffer bb = ByteBuffer.wrap(c.out, c.outPos, c.outLen - c.outPos);
            int n = c.ch.write(bb);
            if (n <= 0) {
                SelectionKey k = c.ch.keyFor(selector);
                if (k != null && k.isValid()) k.interestOps(SelectionKey.OP_READ | SelectionKey.OP_WRITE);
                return;
            }
            c.outPos += n;
        }
        c.outPos = 0;
        c.outLen = 0;
        SelectionKey k = c.ch.keyFor(selector);
        if (k != null && k.isValid()) k.interestOps(SelectionKey.OP_READ);
    }

    void close(Conn c) {
        conns.remove(c.ch);
        SelectionKey k = c.ch.keyFor(selector);
        if (k != null) k.cancel();
        try { c.ch.close(); } catch (IOException ignored) { }
    }

    static int indexOfHeaderEnd(byte[] b, int len) {
        for (int i = 0; i + 3 < len; ++i) {
            if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
                return i;
            }
        }
        return -1;
    }

    static int indexOf(byte[] b, int len, byte needle, int from, int limit) {
        for (int i = from; i < limit && i < len; ++i) if (b[i] == needle) return i;
        return -1;
    }

    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 18081;
        new BenchServerFair().run(port);
    }
}
