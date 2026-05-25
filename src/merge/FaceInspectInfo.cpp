#include "merge/FaceInspectInfo.h"

namespace spo {

const char* toString(FaceInspectCandidateState state) {
    switch (state) {
    case FaceInspectCandidateState::InVisibleCandidate:
        return "InVisibleCandidate";
    case FaceInspectCandidateState::InHiddenCandidate:
        return "InHiddenCandidate";
    case FaceInspectCandidateState::InCandidateButNotDisplayed:
        return "InCandidateButNotDisplayed";
    case FaceInspectCandidateState::NotInCandidate:
        return "NotInCandidate";
    }
    return "NotInCandidate";
}

}
