// 对拍用 Java NIO 服务器（JDK 8 兼容；单线程 selector reactor）
//
// 用途：与 cboot 在同一台机器、同一客户端（cboot_bench --client）下对比
//       "Java NIO 事件循环" 与 "C++ IOCP 完成端口" 的每请求开销。
// 说明：
//   - plaintext 负载下 JDK 21 与 JDK 8 的 NIO 吞吐同量级（Loom 只改善阻塞负载）
//   - 本服务是最小实现（无框架），因此结果偏向 Java 的最优情况
// 用法：java BenchServer [port=18080]

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

public class BenchServer {
    private static final byte[] CRLFCRLF = {'\r', '\n', '\r', '\n'};
    private static final byte[] PONG = response("text/plain; charset=utf-8", "pong");
    private static final byte[] JSON = response("application/json",
            "{\"message\":\"hello, world!\",\"id\":42}");

    private final Selector selector;
    private final Map<SocketChannel, Conn> conns = new HashMap<SocketChannel, Conn>();

    static byte[] response(String type, String body) {
        byte[] b = body.getBytes(StandardCharsets.UTF_8);
        String head = "HTTP/1.1 200 OK\r\nContent-Type: " + type
                + "\r\nContent-Length: " + b.length
                + "\r\nConnection: keep-alive\r\n\r\n";
        byte[] h = head.getBytes(StandardCharsets.UTF_8);
        byte[] out = new byte[h.length + b.length];
        System.arraycopy(h, 0, out, 0, h.length);
        System.arraycopy(b, 0, out, h.length, b.length);
        return out;
    }

    static final class Conn {
        final SocketChannel ch;
        byte[] in = new byte[8192];
        int inLen;
        byte[] out = new byte[8192];
        int outLen;
        int outPos;

        Conn(SocketChannel ch) { this.ch = ch; }

        void appendOut(byte[] data) {
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

    BenchServer() throws IOException {
        selector = Selector.open();
    }

    void run(int port) throws IOException {
        ServerSocketChannel ssc = ServerSocketChannel.open();
        ssc.bind(new InetSocketAddress(port), 1024);
        ssc.configureBlocking(false);
        ssc.register(selector, SelectionKey.OP_ACCEPT);
        System.out.println("java bench server listening on " + port);
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
        ByteBuffer buf = ByteBuffer.allocate(8192);
        int n = c.ch.read(buf);
        if (n < 0) { close(c); return; }
        if (n == 0) return;
        if (c.inLen + n > c.in.length) {
            int cap = c.in.length;
            while (cap < c.inLen + n) cap <<= 1;
            byte[] grown = new byte[cap];
            System.arraycopy(c.in, 0, grown, 0, c.inLen);
            c.in = grown;
        }
        System.arraycopy(buf.array(), 0, c.in, c.inLen, n);
        c.inLen += n;

        while (true) {
            int he = indexOfHeaderEnd(c.in, c.inLen);
            if (he < 0) break;
            // 请求行：METHOD SP TARGET SP VERSION
            int sp1 = indexOf(c.in, c.inLen, (byte) ' ', 0, he);
            int sp2 = sp1 < 0 ? -1 : indexOf(c.in, c.inLen, (byte) ' ', sp1 + 1, he);
            if (sp1 < 0 || sp2 < 0) { close(c); return; }
            String target = new String(c.in, sp1 + 1, sp2 - sp1 - 1,
                    StandardCharsets.US_ASCII);
            c.consume(he + 4);
            boolean isJson = target.startsWith("/json");
            c.appendOut(isJson ? JSON : PONG);
        }
        flush(c);
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
            if (b[i] == CRLFCRLF[0] && b[i + 1] == CRLFCRLF[1]
                    && b[i + 2] == CRLFCRLF[2] && b[i + 3] == CRLFCRLF[3]) return i;
        }
        return -1;
    }

    static int indexOf(byte[] b, int len, byte needle, int from, int limit) {
        for (int i = from; i < limit && i < len; ++i) if (b[i] == needle) return i;
        return -1;
    }

    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 18080;
        new BenchServer().run(port);
    }
}
