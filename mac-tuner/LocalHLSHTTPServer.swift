import Foundation
import Network

/// Servidor HTTP solo en loopback para que AVPlayer lea la playlist HLS viva.
final class LocalHLSHTTPServer {
    private let queue = DispatchQueue(label: "com.padtv.pt230tuner.hls-http")
    private var listener: NWListener?

    func start(root: URL, ready: @escaping (Result<URL, Error>) -> Void) {
        do {
            let parameters = NWParameters.tcp
            parameters.requiredLocalEndpoint = .hostPort(
                host: NWEndpoint.Host("127.0.0.1"),
                port: .any
            )
            let listener = try NWListener(using: parameters)
            self.listener = listener
            var didReportReady = false

            listener.stateUpdateHandler = { state in
                switch state {
                case .ready where !didReportReady:
                    didReportReady = true
                    guard let port = listener.port?.rawValue,
                          let url = URL(string: "http://127.0.0.1:\(port)/live.m3u8") else {
                        ready(.failure(HTTPServerError.noPort))
                        return
                    }
                    ready(.success(url))
                case .failed(let error):
                    if !didReportReady { ready(.failure(error)) }
                default:
                    break
                }
            }
            listener.newConnectionHandler = { [weak self] connection in
                self?.serve(connection, root: root)
            }
            listener.start(queue: queue)
        } catch {
            ready(.failure(error))
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    private func serve(_ connection: NWConnection, root: URL) {
        connection.start(queue: queue)
        var request = Data()

        func receiveMore() {
            connection.receive(minimumIncompleteLength: 1, maximumLength: 8192) {
                [weak self] data, _, isComplete, error in
                if let data { request.append(data) }
                if request.range(of: Data("\r\n\r\n".utf8)) != nil {
                    self?.respond(to: connection, request: request, root: root)
                } else if isComplete || error != nil {
                    connection.cancel()
                } else {
                    receiveMore()
                }
            }
        }
        receiveMore()
    }

    private func respond(to connection: NWConnection, request: Data, root: URL) {
        guard let text = String(data: request, encoding: .utf8),
              let firstLine = text.components(separatedBy: "\r\n").first else {
            send(status: "400 Bad Request", body: Data("bad request".utf8),
                 mime: "text/plain", to: connection)
            return
        }
        let fields = firstLine.split(whereSeparator: \.isWhitespace)
        guard fields.count >= 2, fields[0] == "GET" || fields[0] == "HEAD" else {
            send(status: "405 Method Not Allowed", body: Data(),
                 mime: "text/plain", to: connection)
            return
        }

        let target = String(fields[1]).components(separatedBy: "?").first ?? "/"
        let decoded = target.removingPercentEncoding ?? target
        let relative = String(decoded.drop(while: { $0 == "/" }))
        guard !relative.isEmpty,
              !relative.split(separator: "/").contains("..") else {
            send(status: "404 Not Found", body: Data(), mime: "text/plain", to: connection)
            return
        }

        let file = root.appendingPathComponent(relative).standardizedFileURL
        let rootPath = root.standardizedFileURL.path + "/"
        guard file.path.hasPrefix(rootPath),
              let body = try? Data(contentsOf: file) else {
            send(status: "404 Not Found", body: Data("not found".utf8),
                 mime: "text/plain", to: connection)
            return
        }

        let mime: String
        switch file.pathExtension.lowercased() {
        case "m3u8": mime = "application/vnd.apple.mpegurl"
        case "ts": mime = "video/mp2t"
        default: mime = "application/octet-stream"
        }
        send(status: "200 OK", body: fields[0] == "HEAD" ? Data() : body,
             mime: mime, contentLength: body.count, to: connection)
    }

    private func send(
        status: String,
        body: Data,
        mime: String,
        contentLength: Int? = nil,
        to connection: NWConnection
    ) {
        let length = contentLength ?? body.count
        let header = Data((
            "HTTP/1.1 \(status)\r\n" +
            "Content-Type: \(mime)\r\n" +
            "Content-Length: \(length)\r\n" +
            "Cache-Control: no-store, no-cache, must-revalidate\r\n" +
            "Connection: close\r\n\r\n"
        ).utf8)
        var response = header
        response.append(body)
        connection.send(content: response, completion: .contentProcessed { _ in
            connection.cancel()
        })
    }

    private enum HTTPServerError: Error {
        case noPort
    }
}
