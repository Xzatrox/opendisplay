// MacReceiver — listens for incoming connections from a Windows sender,
// decodes H.264 video and renders it full-screen via Metal.
//
// This reverses the Mac's normal role: instead of sending to an iOS device,
// the Mac acts as a display target for a Windows (or other) sender.
//
// Pipeline:  NWListener (TCP :9000) -> deframe -> decode -> Metal render
// Wire protocol:  [4-byte big-endian length][payload]
//   - Video: telemetry JSON prefix + Annex B H.264
//   - Control: pure JSON (< 32KB, starts with '{', no NUL)

import Foundation
import Network
import AppKit
import VideoToolbox
import CoreMedia
import CoreVideo
import Metal

/// Mac receiver mode — listens on TCP port 9000, decodes H.264, renders via Metal.
/// Reuses the iOS PhoneReceiver's deframing and decode logic adapted for macOS.
@MainActor
final class MacReceiver: ObservableObject {
    @Published var status: String = "Waiting for sender…"
    @Published var connected: Bool = false

    // MARK: - Per-install identity

    /// Stable per-install UUID, stored in UserDefaults. Advertised in Bonjour
    /// TXT and sent in every hello so the sender can recognize this Mac across
    /// reconnects and transports.
    static let installID: String = {
        let key = "macReceiverInstallID"
        if let existing = UserDefaults.standard.string(forKey: key) {
            return existing
        }
        let fresh = UUID().uuidString
        UserDefaults.standard.set(fresh, forKey: key)
        return fresh
    }()

    // MARK: - Networking

    private var listener: NWListener?
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "macReceiver.network")
    private var port: UInt16 = 9000

    // MARK: - H.264 Decoder + Metal Renderer

    private var decoder: VTDecompressionSession?
    private var formatDesc: CMVideoFormatDescription?
    private var sps: Data?
    private var pps: Data?
    private var vps: Data?  // HEVC Video Parameter Set
    private var isHevc: Bool = false  // Auto-detected from NAL types
    private var renderer: MetalVideoRenderer?
    private var lastKeyframeRequest = Date.distantPast
    private var decodeErrorCount = 0

    // MARK: - Input Forwarding

    /// Captures mouse/trackpad events on the receiver window and forwards
    /// them as touch/scroll control messages to the Windows sender.
    private let inputHandler = MacReceiverInputHandler()

    // MARK: - Start / Stop

    /// Start listening on the configured port and advertise via Bonjour.
    func start(port: UInt16 = 9000) {
        self.port = port
        setupInputHandler()
        startListener()
    }

    /// Stop listening, tear down connections.
    func stop() {
        inputHandler.detach()
        connection?.cancel()
        connection = nil
        listener?.cancel()
        listener = nil
        if let session = decoder {
            VTDecompressionSessionInvalidate(session)
            decoder = nil
        }
        formatDesc = nil
        sps = nil
        pps = nil
        connected = false
        status = "Stopped"
        receiverWindow?.close()
        receiverWindow = nil
        renderer = nil
    }

    // MARK: - Listener setup

    private func startListener() {
        do {
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            let params = NWParameters(tls: nil, tcp: tcp)
            params.allowLocalEndpointReuse = true
            params.serviceClass = .interactiveVideo
            listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        } catch {
            DispatchQueue.main.async {
                self.status = "Listener failed: \(error.localizedDescription)"
            }
            return
        }

        // Advertise via Bonjour with TXT record: id, pv, device="Mac"
        listener?.service = advertisedService

        listener?.newConnectionHandler = { [weak self] conn in
            guard let self else { return }
            Log.info("MacReceiver: new connection from \(String(describing: conn.endpoint))")
            Task { @MainActor [weak self] in
                guard let self else { return }
                // Replace any existing connection.
                self.connection?.cancel()
                self.connection = conn

                conn.stateUpdateHandler = { [weak self] state in
                    guard let self else { return }
                    switch state {
                    case .ready:
                        Log.info("MacReceiver: connection ready")
                        Task { @MainActor [weak self] in
                            guard let self else { return }
                            self.connected = true
                            self.status = "Connected"
                            self.sendHello(on: conn)
                        }
                    case .failed(let error):
                        Log.info("MacReceiver: connection failed: \(error)")
                        Task { @MainActor [weak self] in self?.handleConnectionLoss() }
                    case .cancelled:
                        Log.info("MacReceiver: connection cancelled")
                        Task { @MainActor [weak self] in self?.handleConnectionLoss() }
                    default:
                        break
                    }
                }
                conn.start(queue: self.queue)
                self.receive(on: conn)
            }
        }

        listener?.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    Log.info("MacReceiver: listening on port \(self.port)")
                    self.status = "Waiting for sender…"
                }
            case .failed(let error):
                Log.info("MacReceiver: listener failed: \(error) — restarting in 1s")
                Task { @MainActor [weak self] in
                    self?.status = "Listener failed — restarting…"
                }
                self.queue.asyncAfter(deadline: .now() + 1) {
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        self.listener?.cancel()
                        self.listener = nil
                        self.startListener()
                    }
                }
            case .cancelled:
                break
            default:
                break
            }
        }

        listener?.start(queue: queue)
    }

    // MARK: - Bonjour advertisement

    private var advertisedService: NWListener.Service {
        var txt = NWTXTRecord()
        txt["id"] = Self.installID
        txt["pv"] = String(WireProtocol.version)
        txt["device"] = "Mac"
        return NWListener.Service(name: macServiceName, type: "_opensidecar._tcp",
                                  domain: nil, txtRecord: txt)
    }

    /// The advertised Bonjour name. Uses the Mac's computer name.
    private var macServiceName: String {
        Host.current().localizedName ?? "Mac"
    }

    // MARK: - Hello message

    /// Send hello with native display resolution, scale, device="Mac".
    private func sendHello(on conn: NWConnection) {
        let screen = NSScreen.main ?? NSScreen.screens.first
        let pixelsWide: Int
        let pixelsHigh: Int
        let scale: Double

        if let screen = screen {
            let backing = screen.backingScaleFactor
            pixelsWide = Int(screen.frame.width * backing)
            pixelsHigh = Int(screen.frame.height * backing)
            scale = Double(backing)
        } else {
            // Fallback if no screen available (headless Mac, unlikely)
            pixelsWide = 2560
            pixelsHigh = 1600
            scale = 2.0
        }

        let hello: [String: Any] = [
            "type": "hello",
            "pixelsWide": pixelsWide,
            "pixelsHigh": pixelsHigh,
            "scale": scale,
            "device": "Mac",
            "id": Self.installID,
            "pv": WireProtocol.version,
        ]

        sendControl(hello, on: conn)
        Log.info("MacReceiver: hello sent (\(pixelsWide)x\(pixelsHigh) @\(scale)x)")
    }

    // MARK: - Connection loss handling

    private func handleConnectionLoss() {
        self.connection = nil
        // Reset decoder state so the next connection starts clean.
        if let session = self.decoder {
            VTDecompressionSessionInvalidate(session)
            self.decoder = nil
        }
        self.formatDesc = nil
        self.sps = nil
        self.pps = nil
        self.vps = nil
        self.isHevc = false
        self.decodeErrorCount = 0
        connected = false
        status = "Connection lost — waiting for sender…"
        receiverWindow?.close()
        receiverWindow = nil
        renderer = nil
        // The listener continues advertising; a new connection will be
        // accepted automatically via newConnectionHandler.
        Log.info("MacReceiver: connection lost, continuing to advertise")
    }

    // MARK: - Receive loop (deframing)

    private var buffer = Data()

    private func receive(on conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 18) {
            [weak self] data, _, isComplete, error in
            guard let self else { return }
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let data, !data.isEmpty {
                    self.buffer.append(data)
                    self.drainFrames()
                }
                if let error {
                    Log.info("MacReceiver: receive error: \(error)")
                    return
                }
                if isComplete {
                    Log.info("MacReceiver: peer closed connection")
                    self.handleConnectionLoss()
                    return
                }
                self.receive(on: conn)
            }
        }
    }

    /// Deframe [4B big-endian length][payload] messages from the buffer.
    private func drainFrames() {
        var cursor = buffer.startIndex
        while buffer.distance(from: cursor, to: buffer.endIndex) >= 4 {
            let lenBytes = buffer[cursor..<buffer.index(cursor, offsetBy: 4)]
            let len = lenBytes.withUnsafeBytes {
                Int(UInt32(bigEndian: $0.loadUnaligned(as: UInt32.self)))
            }
            guard buffer.distance(from: cursor, to: buffer.endIndex) >= 4 + len else { break }
            let start = buffer.index(cursor, offsetBy: 4)
            let end = buffer.index(start, offsetBy: len)
            let payload = Data(buffer[start..<end])
            handlePayload(payload)
            cursor = end
        }
        buffer.removeSubrange(buffer.startIndex..<cursor)
    }

    /// Route a deframed payload to the appropriate handler.
    /// Control messages are JSON (< 32KB, starts with '{', no NUL bytes).
    /// Video frames contain H.264 start codes (contain 0x00 bytes).
    private func handlePayload(_ data: Data) {
        if data.count < 32_768,
           data.first == UInt8(ascii: "{"),
           !data.contains(0x00) {
            handleControlMessage(data)
        } else {
            // Video frame — will be handled in task 12.2
            handleVideoFrame(data)
        }
    }

    // MARK: - Control messages (from sender)

    private func handleControlMessage(_ data: Data) {
        guard let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = obj["type"] as? String else { return }

        switch type {
        case "ping":
            // Respond with pong
            guard let t = obj["t"] as? Double else { return }
            let mt = Date().timeIntervalSince1970 * 1000
            sendControl(["type": "pong", "t": t, "mt": mt])
        case WireMessage.welcome:
            Log.info("MacReceiver: received welcome from sender")
        case WireMessage.updateRequired:
            let message = obj["message"] as? String ?? "Update required"
            Log.info("MacReceiver: updateRequired — \(message)")
            DispatchQueue.main.async {
                self.status = message
            }
        default:
            Log.info("MacReceiver: unknown control message type: \(type)")
        }
    }

    // MARK: - Video frame handling

    /// Parse and decode a video frame from the wire.
    /// Wire format: [JSON telemetry prefix {"cap":ms,"snd":ms}][H.264 Annex B starting with 00 00 00 01]
    private func handleVideoFrame(_ data: Data) {
        // Split on 4-byte start codes (00 00 00 01).
        // Bytes before the FIRST start code are the telemetry prefix JSON.
        var nalus: [Data] = []
        var metaPrefix: Data?
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let bytes = raw.bindMemory(to: UInt8.self)
            var naluStart: Int? = nil
            var firstSC: Int? = nil
            var i = 0
            while i + 4 <= bytes.count {
                if bytes[i] == 0, bytes[i+1] == 0, bytes[i+2] == 0, bytes[i+3] == 1 {
                    if firstSC == nil { firstSC = i }
                    if let s = naluStart, s < i { nalus.append(Data(bytes[s..<i])) }
                    naluStart = i + 4
                    i += 4
                } else {
                    i += 1
                }
            }
            if let s = naluStart, s < bytes.count { nalus.append(Data(bytes[s...])) }
            if let f = firstSC, f > 0 { metaPrefix = Data(bytes[0..<f]) }
        }

        // If no NAL units found, frame is malformed — request keyframe.
        guard !nalus.isEmpty else {
            requestKeyframe()
            return
        }

        // Parse telemetry prefix (optional, used for latency measurement).
        var captureMs: Double?
        if let metaPrefix,
           let meta = try? JSONSerialization.jsonObject(with: metaPrefix) as? [String: Any] {
            captureMs = meta["cap"] as? Double
        }

        // Categorize NAL units.
        // H.264: SPS=7, PPS=8, IDR=5, non-IDR=1, SEI=6
        // HEVC:  VPS=32, SPS=33, PPS=34, IDR_W_RADL=19, IDR_N_LP=20, CRA=21, TRAIL=1
        // NAL type extraction differs: H.264 = first_byte & 0x1F
        //                              HEVC  = (first_byte >> 1) & 0x3F
        var vclNALUs: [Data] = []
        for nalu in nalus {
            guard let first = nalu.first else { continue }

            // Detect codec from first NAL unit type range
            // HEVC NAL types 32-34 are parameter sets; H.264 uses 7-8
            let h264Type = first & 0x1F
            let hevcType = (first >> 1) & 0x3F

            // Auto-detect: if we see HEVC-range parameter sets, switch to HEVC
            if hevcType >= 32 && hevcType <= 34 {
                isHevc = true
            }

            if isHevc {
                switch hevcType {
                case 32: // VPS
                    if vps != nalu { vps = nalu; formatDesc = nil }
                case 33: // SPS
                    if sps != nalu { sps = nalu; formatDesc = nil }
                case 34: // PPS
                    if pps != nalu { pps = nalu; formatDesc = nil }
                case 35, 39, 40: // AUD, SEI prefix, SEI suffix — skip
                    break
                default: // VCL slice
                    vclNALUs.append(nalu)
                }
            } else {
                switch h264Type {
                case 7:  // SPS
                    if sps != nalu { sps = nalu; formatDesc = nil }
                case 8:  // PPS
                    if pps != nalu { pps = nalu; formatDesc = nil }
                case 6:  // SEI — skip
                    break
                default: // VCL slice (IDR=5, non-IDR=1, etc.)
                    vclNALUs.append(nalu)
                }
            }
        }

        // Build format description if needed
        if formatDesc == nil {
            if isHevc, let vps, let sps, let pps {
                buildHevcFormatDescription(vps: vps, sps: sps, pps: pps)
            } else if let sps, let pps {
                buildFormatDescription(sps: sps, pps: pps)
            }
        }

        guard !vclNALUs.isEmpty, let formatDesc else { return }

        // Build AVCC-style buffer: each NALU prefixed with 4-byte big-endian length.
        var avcc = Data(capacity: vclNALUs.reduce(0) { $0 + $1.count + 4 })
        for nalu in vclNALUs {
            var len = UInt32(nalu.count).bigEndian
            avcc.append(Data(bytes: &len, count: 4))
            avcc.append(nalu)
        }

        // Allocate block buffer that owns its memory.
        var blockBuffer: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
                allocator: kCFAllocatorDefault,
                memoryBlock: nil,
                blockLength: avcc.count,
                blockAllocator: kCFAllocatorDefault,
                customBlockSource: nil, offsetToData: 0,
                dataLength: avcc.count, flags: 0,
                blockBufferOut: &blockBuffer) == noErr,
              let blockBuffer else {
            requestKeyframe()
            return
        }
        let copyStatus = avcc.withUnsafeBytes { raw in
            CMBlockBufferReplaceDataBytes(
                with: raw.baseAddress!, blockBuffer: blockBuffer,
                offsetIntoDestination: 0, dataLength: avcc.count)
        }
        guard copyStatus == noErr else {
            requestKeyframe()
            return
        }

        // Create CMSampleBuffer for VideoToolbox.
        var sample: CMSampleBuffer?
        var sizeArr = [avcc.count]
        CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDesc,
            sampleCount: 1,
            sampleTimingEntryCount: 0, sampleTimingArray: nil,
            sampleSizeEntryCount: 1, sampleSizeArray: &sizeArr,
            sampleBufferOut: &sample)

        guard let sample else {
            requestKeyframe()
            return
        }

        // Decode and render.
        decodeAndRender(sample, captureMs: captureMs)
    }

    // MARK: - Format Description

    private func buildFormatDescription(sps: Data, pps: Data) {
        sps.withUnsafeBytes { spsBuf in
            pps.withUnsafeBytes { ppsBuf in
                let ptrs: [UnsafePointer<UInt8>] = [
                    spsBuf.bindMemory(to: UInt8.self).baseAddress!,
                    ppsBuf.bindMemory(to: UInt8.self).baseAddress!
                ]
                let sizes = [sps.count, pps.count]
                var desc: CMVideoFormatDescription?
                let status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: 2,
                    parameterSetPointers: ptrs,
                    parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4,
                    formatDescriptionOut: &desc
                )
                if status == noErr, let desc {
                    self.formatDesc = desc
                    let dims = CMVideoFormatDescriptionGetDimensions(desc)
                    Log.info("MacReceiver: H.264 format: \(dims.width)x\(dims.height)")
                    if let session = self.decoder {
                        VTDecompressionSessionInvalidate(session)
                        self.decoder = nil
                    }
                } else {
                    Log.info("MacReceiver: H.264 format description FAILED: \(status)")
                }
            }
        }
    }

    private func buildHevcFormatDescription(vps: Data, sps: Data, pps: Data) {
        vps.withUnsafeBytes { vpsBuf in
            sps.withUnsafeBytes { spsBuf in
                pps.withUnsafeBytes { ppsBuf in
                    let ptrs: [UnsafePointer<UInt8>] = [
                        vpsBuf.bindMemory(to: UInt8.self).baseAddress!,
                        spsBuf.bindMemory(to: UInt8.self).baseAddress!,
                        ppsBuf.bindMemory(to: UInt8.self).baseAddress!
                    ]
                    let sizes = [vps.count, sps.count, pps.count]
                    var desc: CMVideoFormatDescription?
                    let status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 3,
                        parameterSetPointers: ptrs,
                        parameterSetSizes: sizes,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &desc
                    )
                    if status == noErr, let desc {
                        self.formatDesc = desc
                        let dims = CMVideoFormatDescriptionGetDimensions(desc)
                        Log.info("MacReceiver: HEVC format: \(dims.width)x\(dims.height)")
                        if let session = self.decoder {
                            VTDecompressionSessionInvalidate(session)
                            self.decoder = nil
                        }
                    } else {
                        Log.info("MacReceiver: HEVC format description FAILED: \(status)")
                    }
                }
            }
        }
    }

    // MARK: - VTDecompressionSession

    private func ensureDecoder() {
        guard let formatDesc else { return }
        if let session = decoder {
            if VTDecompressionSessionCanAcceptFormatDescription(session, formatDescription: formatDesc) {
                return
            }
            VTDecompressionSessionInvalidate(session)
            decoder = nil
        }
        // Request NV12 output with Metal compatibility for the renderer.
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        var session: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: nil, formatDescription: formatDesc, decoderSpecification: nil,
            imageBufferAttributes: attrs as CFDictionary, outputCallback: nil,
            decompressionSessionOut: &session)
        if status != noErr {
            Log.info("MacReceiver: VTDecompressionSessionCreate failed: \(status)")
        }
        decoder = session
    }

    /// Synchronous hardware decode — renders the decoded frame via Metal.
    /// On failure: sends keyframe request, keeps displaying last good frame.
    private func decodeAndRender(_ sample: CMSampleBuffer, captureMs: Double?) {
        ensureDecoder()
        guard let session = decoder else {
            requestKeyframe()
            return
        }
        let status = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sample, flags: [], infoFlagsOut: nil
        ) { [weak self] decodeStatus, _, imageBuffer, _, _ in
            guard let self else { return }
            Task { @MainActor [weak self] in
                guard let self else { return }
                if decodeStatus == noErr, let imageBuffer {
                    self.decodeErrorCount = 0
                    self.renderFrame(imageBuffer, captureMs: captureMs)
                } else {
                    self.decodeErrorCount += 1
                    if self.decodeErrorCount % 60 == 1 {
                        Log.info("MacReceiver: decode output error: \(decodeStatus)")
                    }
                    self.requestKeyframe()
                }
            }
        }
        if status != noErr {
            decodeErrorCount += 1
            if decodeErrorCount % 60 == 1 {
                Log.info("MacReceiver: decode call error: \(status) (\(decodeErrorCount) total)")
            }
            requestKeyframe()
        }
    }

    // MARK: - Metal Rendering

    /// Ensure the renderer is initialized and render the decoded pixel buffer.
    private func renderFrame(_ pixelBuffer: CVPixelBuffer, captureMs: Double?) {
        if renderer == nil {
            renderer = MetalVideoRenderer()
            if let renderer {
                Log.info("MacReceiver: Metal renderer initialized")
                // Install the layer on the main thread.
                DispatchQueue.main.async { [weak self] in
                    self?.installRendererLayer(renderer.metalLayer)
                }
            } else {
                Log.info("MacReceiver: Metal renderer initialization failed")
            }
        }
        renderer?.render(pixelBuffer, captureMs: captureMs)
    }

    /// Install the Metal layer into the key window for full-screen display.
    /// Called on main thread.
    private var receiverWindow: NSWindow?

    private func installRendererLayer(_ layer: CAMetalLayer) {
        if receiverWindow == nil {
            let screen = NSScreen.main ?? NSScreen.screens.first
            let frame = screen?.frame ?? NSRect(x: 0, y: 0, width: 1280, height: 800)
            let w = NSWindow(
                contentRect: frame,
                styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView],
                backing: .buffered,
                defer: false)
            w.title = "OpenDisplay — Receiver"
            w.backgroundColor = .black
            w.isReleasedWhenClosed = false
            w.collectionBehavior = [.fullScreenPrimary]
            w.makeKeyAndOrderFront(nil)
            w.toggleFullScreen(nil)
            receiverWindow = w
        }
        guard let window = receiverWindow,
              let contentView = window.contentView else { return }
        contentView.wantsLayer = true
        if let hostLayer = contentView.layer {
            layer.frame = hostLayer.bounds
            layer.autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
            hostLayer.sublayers?.forEach { $0.removeFromSuperlayer() }
            hostLayer.addSublayer(layer)
            Log.info("MacReceiver: renderer layer installed (\(Int(hostLayer.bounds.width))x\(Int(hostLayer.bounds.height)))")
        }
        inputHandler.attach(to: window)
    }

    // MARK: - Keyframe Request

    /// Request a keyframe from the sender. Throttled to at most once per second.
    private func requestKeyframe() {
        let now = Date()
        guard now.timeIntervalSince(lastKeyframeRequest) > 1 else { return }
        lastKeyframeRequest = now
        Log.info("MacReceiver: requesting keyframe (decoder needs sync)")
        sendControl(["type": "kf"])
    }

    // MARK: - Control message sending

    /// Send a JSON control message framed as [4B BE length][JSON payload].
    private func sendControl(_ message: [String: Any], on conn: NWConnection? = nil) {
        guard let conn = conn ?? connection,
              let payload = try? JSONSerialization.data(withJSONObject: message) else {
            return
        }
        var header = UInt32(payload.count).bigEndian
        var frame = Data(bytes: &header, count: 4)
        frame.append(payload)
        conn.send(content: frame, completion: .contentProcessed { error in
            if let error {
                Log.info("MacReceiver: control send error: \(error)")
            }
        })
    }

    // MARK: - Input forwarding

    /// Wire the input handler's callbacks to send control messages.
    private func setupInputHandler() {
        inputHandler.onTouch = { [weak self] phase, x, y in
            self?.sendTouch(phase: phase, x: x, y: y)
        }
        inputHandler.onScroll = { [weak self] dx, dy in
            self?.sendScroll(dx: dx, dy: dy)
        }
    }

    /// Forward local mouse/trackpad input back to the sender as control messages.
    func sendTouch(phase: String, x: Double, y: Double) {
        sendControl(["type": "touch", "phase": phase, "x": x, "y": y])
    }

    /// Forward scroll events to the sender.
    func sendScroll(dx: Double, dy: Double) {
        sendControl(["type": "scroll", "dx": dx, "dy": dy])
    }
}
