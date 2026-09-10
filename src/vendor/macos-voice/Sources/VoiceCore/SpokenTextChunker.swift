import Foundation

/// Turns a streaming markdown reply into short utterances that can be spoken as they arrive.
///
/// Feeding whole replies to a synthesizer means the child waits for the model to finish
/// writing before hearing anything. Feeding it raw markdown means hearing asterisks and file
/// paths. This does both jobs: it emits complete sentences as soon as they are complete, and
/// it says something brief in place of the parts of a reply that are meant to be looked at
/// rather than listened to.
struct SpokenTextChunker: Sendable {
    /// Long enough that ordinary sentences are never cut, short enough that one runaway line
    /// cannot hold the speaker for a minute.
    static let utteranceLimit = 320

    static let codeBlockStandIn = "I've put the code on the screen."
    static let tableStandIn = "I've put a table on the screen."
    /// Emitted between utterances where the reply had a paragraph break, so the voice can
    /// take the breath a reader would. Never first, never last, never doubled.
    static let paragraphBreak = "\u{2029}"

    private var pending = ""
    private var fence: Character?
    private var announcedCode = false
    private var inTable = false
    private var spokenSinceBreak = false
    private var breakPending = false

    init() {}

    /// Feeds the next slice of the reply and returns whatever became speakable because of it.
    mutating func append(_ delta: String) -> [String] {
        guard !delta.isEmpty else { return [] }
        pending += delta
        var utterances: [String] = []

        while let index = pending.firstIndex(of: "\n") {
            let line = String(pending[pending.startIndex..<index])
            pending = String(pending[pending.index(after: index)...])
            utterances.append(contentsOf: emit(consume(line: line, isComplete: true)))
        }

        // The trailing partial line can still yield finished sentences, which is what keeps
        // the voice close behind the text. It cannot be classified as a fence or a table row
        // until it ends, so hold it back while it might become one.
        guard fence == nil, !looksUnfinishedStructure(pending) else { return utterances }
        let spoken = Self.speakable(pending)
        guard !spoken.isEmpty else { return utterances }
        let split = Self.sentences(in: spoken, flushingTail: false)
        guard !split.complete.isEmpty else { return utterances }
        pending = split.remainder
        utterances.append(contentsOf: emit(split.complete.flatMap { Self.clamped($0) }))
        return utterances
    }

    private mutating func emit(_ utterances: [String]) -> [String] {
        guard !utterances.isEmpty else { return [] }
        defer {
            spokenSinceBreak = true
            breakPending = false
        }
        return breakPending && spokenSinceBreak
            ? [Self.paragraphBreak] + utterances : utterances
    }

    /// Flushes whatever is left when the reply ends.
    mutating func finish() -> [String] {
        let line = pending
        pending = ""
        fence = nil
        inTable = false
        announcedCode = false
        defer {
            spokenSinceBreak = false
            breakPending = false
        }
        guard !line.isEmpty else { return [] }
        return emit(consume(line: line, isComplete: false))
    }

    private mutating func consume(line: String, isComplete: Bool) -> [String] {
        let trimmed = line.trimmingCharacters(in: .whitespaces)

        if let active = fence {
            if Self.isFence(trimmed, marker: active) { fence = nil }
            return []
        }
        if isComplete, let marker = Self.fenceMarker(trimmed) {
            fence = marker
            defer { announcedCode = true }
            return announcedCode ? [] : [Self.codeBlockStandIn]
        }
        if Self.isTableRow(trimmed) {
            defer { inTable = true }
            return inTable ? [] : [Self.tableStandIn]
        }
        inTable = false
        if Self.isHorizontalRule(trimmed) {
            breakPending = true
            return []
        }

        let spoken = Self.speakable(line)
        guard !spoken.isEmpty else {
            // A blank line is a paragraph break, and a listener should hear one.
            if trimmed.isEmpty { breakPending = true }
            return []
        }
        // A finished line is a boundary in its own right: a heading or a list item is a whole
        // thought even without a full stop at the end of it.
        return Self.sentences(in: spoken, flushingTail: true).complete.flatMap { Self.clamped($0) }
    }

    /// True while a partial line might still turn out to be a fence or a table row.
    private func looksUnfinishedStructure(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard let first = trimmed.first else { return false }
        return first == "`" || first == "~" || first == "|"
    }

    // MARK: - markdown

    /// Strips the markup a reader sees and a listener should not hear.
    static func speakable(_ line: String) -> String {
        var text = line.trimmingCharacters(in: .whitespaces)
        text = stripLeadingMarkers(text)
        text = stripInline(text)
        text = spokenFlags(text)
        return text.trimmingCharacters(in: .whitespaces)
    }

    /// Reads a command-line flag as words. A run of hyphens is not speech, and Personal Voice
    /// can drop the text around one rather than say it.
    static func spokenFlags(_ line: String) -> String {
        var output = ""
        var index = line.startIndex
        var atWordStart = true
        while index < line.endIndex {
            let character = line[index]
            guard atWordStart, character == "-" else {
                output.append(character)
                atWordStart = character.isWhitespace || "([\"'".contains(character)
                index = line.index(after: index)
                continue
            }
            let dashes = line[index...].prefix(while: { $0 == "-" })
            let rest = line[line.index(index, offsetBy: dashes.count)...]
            guard let next = rest.first, next.isLetter else {
                output.append(contentsOf: dashes)
                index = line.index(index, offsetBy: dashes.count)
                atWordStart = false
                continue
            }
            output += String(repeating: "dash ", count: dashes.count)
            index = line.index(index, offsetBy: dashes.count)
            atWordStart = false
        }
        return output
    }

    private static func stripLeadingMarkers(_ line: String) -> String {
        var text = Substring(line)
        var changed = true
        while changed {
            changed = false
            if text.first == ">" {
                text = text.dropFirst().drop(while: { $0 == " " })
                changed = true
                continue
            }
            let hashes = text.prefix(while: { $0 == "#" })
            if !hashes.isEmpty, text.dropFirst(hashes.count).first == " " {
                text = text.dropFirst(hashes.count).drop(while: { $0 == " " })
                changed = true
                continue
            }
            if let bullet = text.first, bullet == "-" || bullet == "*" || bullet == "+",
               text.dropFirst().first == " " {
                text = text.dropFirst().drop(while: { $0 == " " })
                changed = true
                continue
            }
            // A list number is kept: "there are three reasons" followed by "one", "two",
            // "three" is how the reply reads on the screen, and the count is part of the
            // sense. Only the form is normalized, so "1)" is read the same as "1.".
            let digits = text.prefix(while: \.isNumber)
            if !digits.isEmpty {
                let rest = text.dropFirst(digits.count)
                if let dot = rest.first, dot == "." || dot == ")", rest.dropFirst().first == " " {
                    return digits + ". " + rest.dropFirst().drop(while: { $0 == " " })
                }
            }
        }
        return String(text)
    }

    /// Emphasis and code markers go; links and images are reduced to the words in them.
    private static func stripInline(_ line: String) -> String {
        var output = ""
        var index = line.startIndex
        while index < line.endIndex {
            let character = line[index]
            if character == "!", let text = linkText(in: line, from: index, image: true) {
                index = text.end
                continue
            }
            if character == "[", let text = linkText(in: line, from: index, image: false) {
                output += text.label
                index = text.end
                continue
            }
            if character == "*" || character == "`" || character == "_" || character == "~" {
                index = line.index(after: index)
                continue
            }
            output.append(character)
            index = line.index(after: index)
        }
        return output.replacingOccurrences(of: "  ", with: " ")
    }

    private static func linkText(
        in line: String,
        from start: String.Index,
        image: Bool
    ) -> (label: String, end: String.Index)? {
        var index = start
        if image {
            index = line.index(after: index)
            guard index < line.endIndex, line[index] == "[" else { return nil }
        }
        guard let close = line[index...].firstIndex(of: "]") else { return nil }
        let open = line.index(after: close)
        guard open < line.endIndex, line[open] == "(" else { return nil }
        guard let end = line[open...].firstIndex(of: ")") else { return nil }
        let label = String(line[line.index(after: index)..<close])
        return (label, line.index(after: end))
    }

    // MARK: - sentences

    /// Abbreviations whose full stop does not end a sentence.
    static let abbreviations: Set<String> = [
        "mr", "mrs", "ms", "dr", "prof", "st", "mt", "vs", "etc", "e.g", "i.e", "approx", "fig",
    ]

    static func sentences(
        in text: String,
        flushingTail: Bool
    ) -> (complete: [String], remainder: String) {
        var complete: [String] = []
        var current = ""
        var index = text.startIndex

        while index < text.endIndex {
            let character = text[index]
            current.append(character)
            index = text.index(after: index)
            guard character == "." || character == "!" || character == "?" else { continue }
            // Run out any trailing terminators and closing marks so "..." and `?"` stay whole.
            while index < text.endIndex, ".!?)\"'".contains(text[index]) {
                current.append(text[index])
                index = text.index(after: index)
            }
            let followedByBreak = index == text.endIndex || text[index].isWhitespace
            guard followedByBreak, isSentenceEnd(current) else { continue }
            let next = text[index...].first { !$0.isWhitespace }
            guard isBoundary(current, next: next) else { continue }
            let sentence = current.trimmingCharacters(in: .whitespaces)
            if !sentence.isEmpty { complete.append(sentence) }
            current = ""
            while index < text.endIndex, text[index] == " " {
                index = text.index(after: index)
            }
        }

        let tail = current.trimmingCharacters(in: .whitespaces)
        guard flushingTail else { return (complete, current) }
        if !tail.isEmpty { complete.append(tail) }
        return (complete, "")
    }

    /// Whether what follows a terminator actually starts a new sentence. Without this, the
    /// question mark in `She asked "why?" and left.` ends a sentence in the middle of one.
    /// Nothing after it yet means the reply has not caught up, and the terminator is taken at
    /// face value — that is what lets a sentence be spoken the moment it lands.
    private static func isBoundary(_ current: String, next: Character?) -> Bool {
        guard let next else { return ".!?".contains(current.last ?? " ") }
        if next.isUppercase || next.isNumber { return true }
        return "\"'\u{201C}\u{2018}([".contains(next)
    }

    /// False when the full stop belongs to an abbreviation or a single initial rather than to
    /// the end of a thought.
    private static func isSentenceEnd(_ sentence: String) -> Bool {
        let trimmed = sentence.trimmingCharacters(in: .whitespaces)
        guard trimmed.last == "." else { return true }
        let word = trimmed
            .dropLast()
            .reversed()
            .prefix(while: { $0.isLetter || $0 == "." })
            .reversed()
            .map(String.init)
            .joined()
            .lowercased()
        if word.count == 1 { return false }
        return !abbreviations.contains(word)
    }

    /// Splits an over-long utterance on the last sensible pause below the limit.
    static func clamped(_ utterance: String, limit: Int = utteranceLimit) -> [String] {
        guard utterance.count > limit else { return [utterance] }
        var remaining = Substring(utterance)
        var parts: [String] = []
        while remaining.count > limit {
            let window = remaining.prefix(limit)
            let breakIndex = window.lastIndex(where: { $0 == "," || $0 == ";" || $0 == ":" })
                ?? window.lastIndex(of: " ")
                ?? window.endIndex
            let cut = breakIndex == window.endIndex ? window.endIndex : window.index(after: breakIndex)
            let part = remaining[remaining.startIndex..<cut].trimmingCharacters(in: .whitespaces)
            if !part.isEmpty { parts.append(part) }
            remaining = remaining[cut...]
        }
        let tail = remaining.trimmingCharacters(in: .whitespaces)
        if !tail.isEmpty { parts.append(tail) }
        return parts
    }

    // MARK: - block classification

    private static func fenceMarker(_ line: String) -> Character? {
        guard let marker = line.first, marker == "`" || marker == "~" else { return nil }
        return line.prefix(while: { $0 == marker }).count >= 3 ? marker : nil
    }

    private static func isFence(_ line: String, marker: Character) -> Bool {
        line.prefix(while: { $0 == marker }).count >= 3
    }

    private static func isTableRow(_ line: String) -> Bool {
        line.hasPrefix("|") && line.count > 1
    }

    private static func isHorizontalRule(_ line: String) -> Bool {
        guard line.count >= 3, let marker = line.first, "-*_".contains(marker) else { return false }
        return line.allSatisfy { $0 == marker || $0 == " " }
    }
}
