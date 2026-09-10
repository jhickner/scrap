import Foundation
@testable import VoiceCore
import Testing

@Suite("Spoken text chunker")
struct SpokenTextChunkerTests {

    private func spoken(_ deltas: [String]) -> [String] {
        var chunker = SpokenTextChunker()
        var utterances: [String] = []
        for delta in deltas { utterances.append(contentsOf: chunker.append(delta)) }
        utterances.append(contentsOf: chunker.finish())
        return utterances
    }

    @Test("speaks a sentence as soon as it is finished, not when the reply is")
    func speaksEarly() {
        var chunker = SpokenTextChunker()
        #expect(chunker.append("A volcano is a mountain") == [])
        #expect(chunker.append(" that opens downward. ") == ["A volcano is a mountain that opens downward."])

        #expect(chunker.append("Would you like to build one?") == ["Would you like to build one?"])
        #expect(chunker.finish() == [])
    }

    @Test("splits several sentences arriving in one delta")
    func splitsWithinADelta() {
        #expect(spoken(["Yes. No. Maybe so."]) == ["Yes.", "No.", "Maybe so."])
    }

    @Test("keeps abbreviations, initials and decimals inside their sentence")
    func doesNotSplitOnAbbreviations() {
        #expect(spoken(["Dr. Chen measured 3.5 cm. Then she stopped."])
            == ["Dr. Chen measured 3.5 cm.", "Then she stopped."])
        #expect(spoken(["Ask J. R. Tolkien. He knew."]) == ["Ask J. R. Tolkien.", "He knew."])
    }

    @Test("keeps a question mark inside a quotation with the sentence")
    func keepsClosingMarks() {
        #expect(spoken(["She asked \"why?\" and left. Then it rained."])
            == ["She asked \"why?\" and left.", "Then it rained."])
    }

    @Test("says the words, not the markdown")
    func stripsMarkdown() {
        #expect(spoken(["## Volcanoes\n"]) == ["Volcanoes"])
        #expect(spoken(["- **Magma** is `hot` rock.\n"]) == ["Magma is hot rock."])
        #expect(spoken(["1. Read [the guide](http://x.test) first.\n"]) == ["1.", "Read the guide first."])
        #expect(spoken(["> A quote worth hearing.\n"]) == ["A quote worth hearing."])
    }

    @Test("ends a line without a full stop rather than holding it")
    func flushesAtEndOfLine() {
        #expect(spoken(["# Volcanoes\nThey erupt.\n"]) == ["Volcanoes", "They erupt."])
    }

    @Test("says something short in place of a code block instead of reading it")
    func standsInForCode() {
        let utterances = spoken(["Here it is:\n```python\nprint('hi')\nfor i in x:\n```\nTry it.\n"])
        #expect(utterances == ["Here it is:", SpokenTextChunker.codeBlockStandIn, "Try it."])
    }

    @Test("announces a code block only once per reply")
    func announcesCodeOnce() {
        let utterances = spoken(["```\na\n```\nand\n```\nb\n```\n"])
        #expect(utterances == [SpokenTextChunker.codeBlockStandIn, "and"])
    }

    @Test("does not speak a partial line that might still become a fence")
    func holdsBackPossibleFences() {
        var chunker = SpokenTextChunker()
        #expect(chunker.append("```py") == [])
        #expect(chunker.append("thon\nprint(1)\n") == [SpokenTextChunker.codeBlockStandIn])
    }

    @Test("stands in for a table rather than reading its pipes")
    func standsInForTables() {
        let utterances = spoken(["Look:\n| a | b |\n| - | - |\n| 1 | 2 |\nDone.\n"])
        #expect(utterances == ["Look:", SpokenTextChunker.tableStandIn, "Done."])
    }

    @Test("skips horizontal rules and empty lines, keeping one paragraph break")
    func skipsRules() {
        let pause = SpokenTextChunker.paragraphBreak
        #expect(spoken(["One.\n\n---\n\nTwo.\n"]) == ["One.", pause, "Two."])
    }

    @Test("reads list numbers but not bullets")
    func readsListNumbers() {
        #expect(spoken(["1. A line drawn in the sand\n"]) == ["1.", "A line drawn in the sand"])
        #expect(spoken(["2) Second\n"]) == ["2.", "Second"])
        #expect(spoken(["- A bullet\n"]) == ["A bullet"])
    }

    @Test("marks paragraph breaks between sentences but never at the edges")
    func paragraphBreaks() {
        let pause = SpokenTextChunker.paragraphBreak
        #expect(spoken(["\n\nOne. Two.\n\nThree.\n\n"]) == ["One.", "Two.", pause, "Three."])
        #expect(spoken(["One.\n", "\n", "Two."]) == ["One.", pause, "Two."])
        #expect(spoken(["One. Two."]) == ["One.", "Two."])
    }

    @Test("breaks an over-long sentence at a pause so it cannot hold the speaker")
    func clampsLongUtterances() {
        let long = String(repeating: "word, ", count: 120) + "end."
        let utterances = spoken([long + "\n"])
        #expect(utterances.count > 1)
        #expect(utterances.allSatisfy { $0.count <= SpokenTextChunker.utteranceLimit })
        #expect(utterances.last?.hasSuffix("end.") == true)
    }

    @Test("says nothing for a reply that is only markup")
    func silentOnMarkupOnly() {
        #expect(spoken(["---\n\n"]) == [])
    }
    @Test("reads a flag as words rather than a run of hyphens")
    func speaksFlags() {
        #expect(spoken(["Run `grok update --check` or `-v` now. "])
            == ["Run grok update dash dash check or dash v now."])
    }

    @Test("leaves hyphens that are part of a word or a range alone")
    func keepsHyphens() {
        #expect(spoken(["A check-only flag, pages 3-5. "]) == ["A check-only flag, pages 3-5."])
    }

}
