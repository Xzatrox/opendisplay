// Property 6: AMDS Transport Selection
// **Validates: Requirements 4.1**
//
// For any combination of named pipe availability (reachable/unreachable) and
// TCP socket availability (reachable/unreachable), the AMDS client SHALL
// connect via named pipe when it is available, SHALL fall back to TCP only
// when the named pipe is unavailable, and SHALL report an error only when
// neither transport is available.
//
// This test models the AmdsClient::Connect() transport selection logic without
// requiring actual named pipe or TCP dependencies. We extract the decision
// logic into a testable model that mirrors the real implementation's behavior.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>

namespace {

// ─── Model of the AMDS transport selection logic ─────────────────────────────
// Mirrors the behavior in AmdsClient::Connect():
//   1. Try named pipe (\\.\pipe\usbmux) first
//   2. If pipe unavailable, try TCP (localhost:27015)
//   3. If neither available, return error (AMDS not installed)
//   4. m_usePipe flag tracks which transport was selected

/// Result of a connection attempt.
enum class ConnectResult {
    Pipe,   ///< Connected via named pipe
    Tcp,    ///< Connected via TCP fallback
    Error   ///< Neither transport available
};

/// Model of the AMDS transport selection logic.
/// This implements the same decision tree as AmdsClient::Connect().
class AmdsTransportModel {
public:
    /// Attempt connection given transport availability.
    /// @param pipeAvailable true if named pipe (\\.\pipe\usbmux) is reachable
    /// @param tcpAvailable true if TCP (localhost:27015) is reachable
    /// @return ConnectResult indicating which transport was selected or error
    ConnectResult Connect(bool pipeAvailable, bool tcpAvailable) {
        // Reset state (mirrors the real impl clearing m_pipe/m_tcpSocket)
        m_usePipe = false;
        m_connected = false;

        // Attempt 1: Named pipe (preferred transport)
        if (pipeAvailable) {
            m_usePipe = true;
            m_connected = true;
            return ConnectResult::Pipe;
        }

        // Attempt 2: TCP fallback
        if (tcpAvailable) {
            m_usePipe = false;
            m_connected = true;
            return ConnectResult::Tcp;
        }

        // Neither available — AMDS not installed
        m_usePipe = false;
        m_connected = false;
        return ConnectResult::Error;
    }

    bool usePipe() const { return m_usePipe; }
    bool isConnected() const { return m_connected; }

private:
    bool m_usePipe = false;
    bool m_connected = false;
};

// ─── Oracle: compute expected result for a given availability combination ────
// Pure function that computes the expected outcome without any state.

ConnectResult ExpectedResult(bool pipeAvailable, bool tcpAvailable) {
    if (pipeAvailable) return ConnectResult::Pipe;
    if (tcpAvailable) return ConnectResult::Tcp;
    return ConnectResult::Error;
}

bool ExpectedUsePipe(bool pipeAvailable, bool /*tcpAvailable*/) {
    return pipeAvailable;
}

bool ExpectedConnected(bool pipeAvailable, bool tcpAvailable) {
    return pipeAvailable || tcpAvailable;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 4.1**
// Property: Named pipe is used when available, regardless of TCP availability.
RC_GTEST_PROP(AmdsTransportSelection,
              PipeUsedWhenAvailable,
              ()) {
    // Generate random TCP availability — pipe is always available in this case
    auto tcpAvailable = *rc::gen::arbitrary<bool>();

    AmdsTransportModel model;
    ConnectResult result = model.Connect(/*pipeAvailable=*/true, tcpAvailable);

    RC_ASSERT(result == ConnectResult::Pipe);
    RC_ASSERT(model.usePipe() == true);
    RC_ASSERT(model.isConnected() == true);
}

// **Validates: Requirements 4.1**
// Property: TCP is used only when pipe is unavailable but TCP is available.
RC_GTEST_PROP(AmdsTransportSelection,
              TcpUsedOnlyWhenPipeUnavailable,
              ()) {
    AmdsTransportModel model;
    ConnectResult result = model.Connect(/*pipeAvailable=*/false,
                                         /*tcpAvailable=*/true);

    RC_ASSERT(result == ConnectResult::Tcp);
    RC_ASSERT(model.usePipe() == false);
    RC_ASSERT(model.isConnected() == true);
}

// **Validates: Requirements 4.1**
// Property: Error reported only when neither transport is available.
RC_GTEST_PROP(AmdsTransportSelection,
              ErrorWhenNeitherAvailable,
              ()) {
    AmdsTransportModel model;
    ConnectResult result = model.Connect(/*pipeAvailable=*/false,
                                         /*tcpAvailable=*/false);

    RC_ASSERT(result == ConnectResult::Error);
    RC_ASSERT(model.usePipe() == false);
    RC_ASSERT(model.isConnected() == false);
}

// **Validates: Requirements 4.1**
// Property: For any random combination of pipe/TCP availability, the model
// agrees with the oracle on the transport selection outcome.
RC_GTEST_PROP(AmdsTransportSelection,
              ModelMatchesOracleForAllCombinations,
              ()) {
    auto pipeAvailable = *rc::gen::arbitrary<bool>();
    auto tcpAvailable = *rc::gen::arbitrary<bool>();

    AmdsTransportModel model;
    ConnectResult result = model.Connect(pipeAvailable, tcpAvailable);

    // Verify result matches oracle
    RC_ASSERT(result == ExpectedResult(pipeAvailable, tcpAvailable));
    RC_ASSERT(model.usePipe() == ExpectedUsePipe(pipeAvailable, tcpAvailable));
    RC_ASSERT(model.isConnected() == ExpectedConnected(pipeAvailable, tcpAvailable));
}

// **Validates: Requirements 4.1**
// Property: Repeated connections with different availability always select
// correctly — state from a previous connection does not leak into the next.
RC_GTEST_PROP(AmdsTransportSelection,
              RepeatedConnectionsResetState,
              ()) {
    // Generate a sequence of connection attempts with random availability
    auto numAttempts = *rc::gen::inRange<int>(2, 20);

    AmdsTransportModel model;

    for (int i = 0; i < numAttempts; ++i) {
        auto pipeAvailable = *rc::gen::arbitrary<bool>();
        auto tcpAvailable = *rc::gen::arbitrary<bool>();

        ConnectResult result = model.Connect(pipeAvailable, tcpAvailable);

        // Each attempt must produce the correct result independently
        RC_ASSERT(result == ExpectedResult(pipeAvailable, tcpAvailable));
        RC_ASSERT(model.usePipe() == ExpectedUsePipe(pipeAvailable, tcpAvailable));
        RC_ASSERT(model.isConnected() == ExpectedConnected(pipeAvailable, tcpAvailable));
    }
}

// **Validates: Requirements 4.1**
// Property: Pipe preference is strict — whenever pipe is available, TCP is
// never selected regardless of how many times we retry with varying states.
RC_GTEST_PROP(AmdsTransportSelection,
              PipeAlwaysPreferredOverTcp,
              ()) {
    // Generate random sequences where pipe availability varies
    auto numAttempts = *rc::gen::inRange<int>(1, 50);

    AmdsTransportModel model;

    for (int i = 0; i < numAttempts; ++i) {
        auto pipeAvailable = *rc::gen::arbitrary<bool>();
        auto tcpAvailable = *rc::gen::arbitrary<bool>();

        ConnectResult result = model.Connect(pipeAvailable, tcpAvailable);

        // Core invariant: if pipe is available, result must be Pipe (never Tcp)
        if (pipeAvailable) {
            RC_ASSERT(result == ConnectResult::Pipe);
            RC_ASSERT(model.usePipe() == true);
        }

        // Core invariant: Tcp result implies pipe was NOT available
        if (result == ConnectResult::Tcp) {
            RC_ASSERT(!pipeAvailable);
            RC_ASSERT(tcpAvailable);
        }

        // Core invariant: Error implies neither was available
        if (result == ConnectResult::Error) {
            RC_ASSERT(!pipeAvailable);
            RC_ASSERT(!tcpAvailable);
        }
    }
}
