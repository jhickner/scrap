import Foundation
@testable import VoiceCore
import Testing

@Suite("Utterance completion")
struct UtteranceCompletionTests {
    private func classify(_ text: String) -> UtteranceCompletion {
        UtteranceClassifier.classify(text)
    }

    @Test("a finished question is waiting for an answer")
    func questionIsComplete() {
        #expect(classify("what is a volcano?") == .complete)
        #expect(classify("Tell me about volcanoes.") == .complete)
        #expect(classify("that is so cool!") == .complete)
    }

    @Test("a thought left hanging is not a turn")
    func danglingWordsAreIncomplete() {
        #expect(classify("what is a") == .incomplete)
        #expect(classify("I want to know about volcanoes and") == .incomplete)
        #expect(classify("can we build one if") == .incomplete)
        #expect(classify("the biggest one is") == .incomplete)
        #expect(classify("I was thinking about") == .incomplete)
    }

    @Test("a dangling conjunction outranks the recognizer's full stop")
    func conjunctionBeatsPunctuation() {

        #expect(classify("we could build a volcano and.") == .incomplete)
        #expect(classify("I like it because?") == .incomplete)
    }

    @Test("trailing fillers mean the child is still thinking")
    func fillersAreIncomplete() {
        #expect(classify("I want to ask about um") == .incomplete)
        #expect(classify("it was like, uh") == .incomplete)
    }

    @Test("a one word answer is whole")
    func standaloneAnswers() {
        #expect(classify("yes") == .complete)
        #expect(classify("no thanks") == .complete)
        #expect(classify("why") == .complete)
    }

    @Test("a bare fragment with no punctuation waits")
    func shortFragmentsWait() {
        #expect(classify("big volcano") == .incomplete)
        #expect(classify("mount saint") == .incomplete)
    }

    @Test("a full clause with no punctuation is neither")
    func unpunctuatedClauseIsNeutral() {
        #expect(classify("I really like learning about volcanoes") == .neutral)
    }

    @Test("a trailing comma is a pause, not an ending")
    func commaIsIncomplete() {
        #expect(classify("first we get the baking soda,") == .incomplete)
    }

    @Test("silence scales with how finished it sounds")
    func factorsOrder() {
        #expect(UtteranceCompletion.complete.silenceFactor < UtteranceCompletion.neutral.silenceFactor)
        #expect(UtteranceCompletion.neutral.silenceFactor < UtteranceCompletion.incomplete.silenceFactor)
    }

    @Test("says nothing about silence")
    func emptyIsNeutral() {
        #expect(classify("   ") == .neutral)
    }
}
