package main

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

type NotebookSearchMode string

const (
	NotebookSearchModeLiteral NotebookSearchMode = "literal"
	NotebookSearchModeRegex   NotebookSearchMode = "regex"
	NotebookSearchModeFuzzy   NotebookSearchMode = "fuzzy"
)

type NotebookSearchSurface string

const (
	NotebookSearchSurfaceContent NotebookSearchSurface = "content"
	NotebookSearchSurfaceOutput  NotebookSearchSurface = "output"
)

type NotebookSearchSurfaceFilter string

const (
	NotebookSearchSurfaceFilterAll     NotebookSearchSurfaceFilter = "all"
	NotebookSearchSurfaceFilterContent NotebookSearchSurfaceFilter = "content"
	NotebookSearchSurfaceFilterOutput  NotebookSearchSurfaceFilter = "output"
)

type NotebookSearchCellTypeFilter string

const (
	NotebookSearchCellTypeFilterAll      NotebookSearchCellTypeFilter = "all"
	NotebookSearchCellTypeFilterCommand  NotebookSearchCellTypeFilter = "command"
	NotebookSearchCellTypeFilterScript   NotebookSearchCellTypeFilter = "script"
	NotebookSearchCellTypeFilterMarkdown NotebookSearchCellTypeFilter = "markdown"
)

const (
	defaultNotebookSearchLimit = 200
	maxNotebookSearchLimit     = 500
	fuzzyNoScore               = -1 << 30
	fuzzyGapPenalty            = 8
)

type NotebookSearchOptions struct {
	Query          string
	Mode           NotebookSearchMode
	CaseSensitive  bool
	Limit          int
	SurfaceFilter  NotebookSearchSurfaceFilter
	CellTypeFilter NotebookSearchCellTypeFilter
}

type NotebookSearchRange struct {
	Start int `json:"start"`
	End   int `json:"end"`
}

type NotebookSearchResult struct {
	CellID       string                `json:"cell_id"`
	CellType     string                `json:"cell_type"`
	Surface      NotebookSearchSurface `json:"surface"`
	CellPosition int                   `json:"cell_position"`
	LineNumber   int                   `json:"line_number"`
	StartColumn  int                   `json:"start_column"`
	EndColumn    int                   `json:"end_column"`
	LineText     string                `json:"line_text"`
	Ranges       []NotebookSearchRange `json:"ranges"`
	Score        int                   `json:"score,omitempty"`
}

type NotebookSearchResponse struct {
	Query          string                       `json:"query"`
	Mode           NotebookSearchMode           `json:"mode"`
	CaseSensitive  bool                         `json:"case_sensitive"`
	SurfaceFilter  NotebookSearchSurfaceFilter  `json:"surface_filter"`
	CellTypeFilter NotebookSearchCellTypeFilter `json:"cell_type_filter"`
	Total          int                          `json:"total"`
	Truncated      bool                         `json:"truncated"`
	Results        []NotebookSearchResult       `json:"results"`
}

type notebookSearchDocument struct {
	CellID       string
	CellType     string
	Surface      NotebookSearchSurface
	CellPosition int
	Text         string
}

type notebookSearchLineMatch struct {
	Ranges []NotebookSearchRange
	Score  int
}

func SearchNotebook(cells []CellRow, options NotebookSearchOptions) (NotebookSearchResponse, error) {
	mode, err := normalizeNotebookSearchMode(options.Mode)
	if err != nil {
		return NotebookSearchResponse{}, err
	}

	surfaceFilter, err := normalizeNotebookSearchSurfaceFilter(options.SurfaceFilter)
	if err != nil {
		return NotebookSearchResponse{}, err
	}

	cellTypeFilter, err := normalizeNotebookSearchCellTypeFilter(options.CellTypeFilter)
	if err != nil {
		return NotebookSearchResponse{}, err
	}

	query := strings.TrimSpace(options.Query)
	limit := normalizeNotebookSearchLimit(options.Limit)
	response := NotebookSearchResponse{
		Query:          query,
		Mode:           mode,
		CaseSensitive:  options.CaseSensitive,
		SurfaceFilter:  surfaceFilter,
		CellTypeFilter: cellTypeFilter,
		Results:        []NotebookSearchResult{},
	}
	if query == "" {
		return response, nil
	}

	docs := buildNotebookSearchDocuments(cells, surfaceFilter, cellTypeFilter)
	results := make([]NotebookSearchResult, 0, 32)

	switch mode {
	case NotebookSearchModeLiteral:
		results = searchNotebookLiteral(docs, query, options.CaseSensitive)
	case NotebookSearchModeRegex:
		results, err = searchNotebookRegex(docs, query, options.CaseSensitive)
		if err != nil {
			return response, err
		}
	case NotebookSearchModeFuzzy:
		results = searchNotebookFuzzy(docs, query, options.CaseSensitive)
	default:
		return response, fmt.Errorf("unsupported search mode: %s", mode)
	}

	response.Total = len(results)
	if len(results) > limit {
		response.Truncated = true
		results = results[:limit]
	}
	response.Results = results
	return response, nil
}

func normalizeNotebookSearchMode(mode NotebookSearchMode) (NotebookSearchMode, error) {
	if mode == "" {
		return NotebookSearchModeLiteral, nil
	}
	switch mode {
	case NotebookSearchModeLiteral, NotebookSearchModeRegex, NotebookSearchModeFuzzy:
		return mode, nil
	default:
		return "", fmt.Errorf("invalid search mode: %s", mode)
	}
}

func normalizeNotebookSearchSurfaceFilter(filter NotebookSearchSurfaceFilter) (NotebookSearchSurfaceFilter, error) {
	if filter == "" {
		return NotebookSearchSurfaceFilterAll, nil
	}
	switch filter {
	case NotebookSearchSurfaceFilterAll, NotebookSearchSurfaceFilterContent, NotebookSearchSurfaceFilterOutput:
		return filter, nil
	default:
		return "", fmt.Errorf("invalid surface filter: %s", filter)
	}
}

func normalizeNotebookSearchCellTypeFilter(filter NotebookSearchCellTypeFilter) (NotebookSearchCellTypeFilter, error) {
	if filter == "" {
		return NotebookSearchCellTypeFilterAll, nil
	}
	switch filter {
	case NotebookSearchCellTypeFilterAll, NotebookSearchCellTypeFilterCommand, NotebookSearchCellTypeFilterScript, NotebookSearchCellTypeFilterMarkdown:
		return filter, nil
	default:
		return "", fmt.Errorf("invalid cell type filter: %s", filter)
	}
}

func normalizeNotebookSearchLimit(limit int) int {
	if limit <= 0 {
		return defaultNotebookSearchLimit
	}
	if limit > maxNotebookSearchLimit {
		return maxNotebookSearchLimit
	}
	return limit
}

func buildNotebookSearchDocuments(cells []CellRow, surfaceFilter NotebookSearchSurfaceFilter, cellTypeFilter NotebookSearchCellTypeFilter) []notebookSearchDocument {
	docs := make([]notebookSearchDocument, 0, len(cells)*2)
	for _, cell := range cells {
		if !notebookSearchMatchesCellType(cell.Type, cellTypeFilter) {
			continue
		}
		switch cell.Type {
		case "markdown":
			if cell.Content != "" && notebookSearchMatchesSurface(NotebookSearchSurfaceContent, surfaceFilter) {
				docs = append(docs, notebookSearchDocument{
					CellID:       cell.ID,
					CellType:     cell.Type,
					Surface:      NotebookSearchSurfaceContent,
					CellPosition: cell.Position,
					Text:         normalizeNotebookSearchText(cell.Content),
				})
			}
		case "command", "script":
			if cell.Content != "" && notebookSearchMatchesSurface(NotebookSearchSurfaceContent, surfaceFilter) {
				docs = append(docs, notebookSearchDocument{
					CellID:       cell.ID,
					CellType:     cell.Type,
					Surface:      NotebookSearchSurfaceContent,
					CellPosition: cell.Position,
					Text:         normalizeNotebookSearchText(cell.Content),
				})
			}
			if len(cell.Output) > 0 && notebookSearchMatchesSurface(NotebookSearchSurfaceOutput, surfaceFilter) {
				docs = append(docs, notebookSearchDocument{
					CellID:       cell.ID,
					CellType:     cell.Type,
					Surface:      NotebookSearchSurfaceOutput,
					CellPosition: cell.Position,
					Text:         normalizeNotebookSearchText(outputVisibleText(string(cell.Output))),
				})
			}
		}
	}
	return docs
}

func notebookSearchMatchesSurface(surface NotebookSearchSurface, filter NotebookSearchSurfaceFilter) bool {
	return filter == NotebookSearchSurfaceFilterAll || NotebookSearchSurfaceFilter(surface) == filter
}

func notebookSearchMatchesCellType(cellType string, filter NotebookSearchCellTypeFilter) bool {
	return filter == NotebookSearchCellTypeFilterAll || NotebookSearchCellTypeFilter(cellType) == filter
}

func normalizeNotebookSearchText(text string) string {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	return text
}

func searchNotebookLiteral(docs []notebookSearchDocument, query string, caseSensitive bool) []NotebookSearchResult {
	queryRunes := []rune(query)
	results := make([]NotebookSearchResult, 0, 32)
	for _, doc := range docs {
		lines := strings.Split(doc.Text, "\n")
		for lineIdx, line := range lines {
			matches := findLiteralLineMatches([]rune(line), queryRunes, caseSensitive)
			for _, match := range matches {
				results = append(results, notebookSearchResultFromLineMatch(doc, lineIdx+1, line, match))
			}
		}
	}
	return results
}

func searchNotebookRegex(docs []notebookSearchDocument, query string, caseSensitive bool) ([]NotebookSearchResult, error) {
	pattern := query
	if !caseSensitive {
		pattern = "(?i)" + query
	}
	re, err := regexp.Compile(pattern)
	if err != nil {
		return nil, fmt.Errorf("invalid regex: %w", err)
	}
	if re.MatchString("") {
		return nil, fmt.Errorf("regex must not match empty strings")
	}

	results := make([]NotebookSearchResult, 0, 32)
	for _, doc := range docs {
		lines := strings.Split(doc.Text, "\n")
		for lineIdx, line := range lines {
			matches := findRegexLineMatches(line, re)
			for _, match := range matches {
				results = append(results, notebookSearchResultFromLineMatch(doc, lineIdx+1, line, match))
			}
		}
	}
	return results, nil
}

func searchNotebookFuzzy(docs []notebookSearchDocument, query string, caseSensitive bool) []NotebookSearchResult {
	results := make([]NotebookSearchResult, 0, 32)
	for _, doc := range docs {
		lines := strings.Split(doc.Text, "\n")
		for lineIdx, line := range lines {
			match, ok := findFuzzyLineMatch(line, query, caseSensitive)
			if !ok {
				continue
			}
			results = append(results, notebookSearchResultFromLineMatch(doc, lineIdx+1, line, match))
		}
	}

	sort.SliceStable(results, func(i, j int) bool {
		if results[i].Score != results[j].Score {
			return results[i].Score > results[j].Score
		}
		spanI := results[i].EndColumn - results[i].StartColumn
		spanJ := results[j].EndColumn - results[j].StartColumn
		if spanI != spanJ {
			return spanI < spanJ
		}
		if results[i].CellPosition != results[j].CellPosition {
			return results[i].CellPosition < results[j].CellPosition
		}
		if results[i].LineNumber != results[j].LineNumber {
			return results[i].LineNumber < results[j].LineNumber
		}
		return results[i].StartColumn < results[j].StartColumn
	})
	return results
}

func notebookSearchResultFromLineMatch(doc notebookSearchDocument, lineNumber int, line string, match notebookSearchLineMatch) NotebookSearchResult {
	startCol, endCol := notebookSearchRangeColumns(match.Ranges)
	return NotebookSearchResult{
		CellID:       doc.CellID,
		CellType:     doc.CellType,
		Surface:      doc.Surface,
		CellPosition: doc.CellPosition,
		LineNumber:   lineNumber,
		StartColumn:  startCol,
		EndColumn:    endCol,
		LineText:     line,
		Ranges:       match.Ranges,
		Score:        match.Score,
	}
}

func notebookSearchRangeColumns(ranges []NotebookSearchRange) (int, int) {
	if len(ranges) == 0 {
		return 0, 0
	}
	start := ranges[0].Start + 1
	end := ranges[0].End
	for _, r := range ranges[1:] {
		if r.Start+1 < start {
			start = r.Start + 1
		}
		if r.End > end {
			end = r.End
		}
	}
	return start, end
}

func findLiteralLineMatches(lineRunes, queryRunes []rune, caseSensitive bool) []notebookSearchLineMatch {
	if len(lineRunes) == 0 || len(queryRunes) == 0 || len(queryRunes) > len(lineRunes) {
		return nil
	}

	matches := make([]notebookSearchLineMatch, 0, 2)
	for idx := 0; idx <= len(lineRunes)-len(queryRunes); idx++ {
		if literalRunesEqual(lineRunes[idx:idx+len(queryRunes)], queryRunes, caseSensitive) {
			matches = append(matches, notebookSearchLineMatch{
				Ranges: []NotebookSearchRange{{Start: idx, End: idx + len(queryRunes)}},
				Score:  len(queryRunes) * 100,
			})
		}
	}
	return matches
}

func literalRunesEqual(line, query []rune, caseSensitive bool) bool {
	if len(line) != len(query) {
		return false
	}
	for i := range query {
		if !searchRunesEqual(line[i], query[i], caseSensitive) {
			return false
		}
	}
	return true
}

func searchRunesEqual(a, b rune, caseSensitive bool) bool {
	if caseSensitive {
		return a == b
	}
	return unicode.ToLower(a) == unicode.ToLower(b)
}

func findRegexLineMatches(line string, re *regexp.Regexp) []notebookSearchLineMatch {
	byteMatches := re.FindAllStringIndex(line, -1)
	if len(byteMatches) == 0 {
		return nil
	}

	results := make([]notebookSearchLineMatch, 0, len(byteMatches))
	for _, byteMatch := range byteMatches {
		start := byteOffsetToRuneIndex(line, byteMatch[0])
		end := byteOffsetToRuneIndex(line, byteMatch[1])
		if end <= start {
			continue
		}
		results = append(results, notebookSearchLineMatch{
			Ranges: []NotebookSearchRange{{Start: start, End: end}},
			Score:  (end - start) * 100,
		})
	}
	return results
}

func byteOffsetToRuneIndex(text string, byteOffset int) int {
	if byteOffset <= 0 {
		return 0
	}
	if byteOffset >= len(text) {
		return utf8.RuneCountInString(text)
	}
	return utf8.RuneCountInString(text[:byteOffset])
}

func findFuzzyLineMatch(line, query string, caseSensitive bool) (notebookSearchLineMatch, bool) {
	lineRunes := []rune(line)
	if len(lineRunes) == 0 {
		return notebookSearchLineMatch{}, false
	}

	tokens := fuzzyQueryTokens(query)
	if len(tokens) == 0 {
		return notebookSearchLineMatch{}, false
	}

	allPositions := make([]int, 0, len([]rune(query)))
	totalScore := 0
	searchStart := 0
	for _, token := range tokens {
		tokenRunes := []rune(token)
		match, ok := bestFuzzyTokenMatch(lineRunes, tokenRunes, searchStart, caseSensitive)
		if !ok {
			return notebookSearchLineMatch{}, false
		}
		allPositions = append(allPositions, match.positions...)
		totalScore += match.score
		searchStart = match.positions[len(match.positions)-1] + 1
	}

	if len(allPositions) == 0 {
		return notebookSearchLineMatch{}, false
	}

	sort.Ints(allPositions)
	ranges := mergeFuzzyPositions(allPositions)
	if len(ranges) == 0 {
		return notebookSearchLineMatch{}, false
	}

	return notebookSearchLineMatch{
		Ranges: ranges,
		Score:  totalScore,
	}, true
}

type fuzzyTokenMatch struct {
	positions []int
	score     int
}

func fuzzyQueryTokens(query string) []string {
	return strings.Fields(strings.TrimSpace(query))
}

func bestFuzzyTokenMatch(lineRunes, tokenRunes []rune, searchStart int, caseSensitive bool) (fuzzyTokenMatch, bool) {
	if len(tokenRunes) == 0 || searchStart >= len(lineRunes) {
		return fuzzyTokenMatch{}, false
	}

	lineLen := len(lineRunes)
	tokenLen := len(tokenRunes)
	parents := make([][]int, tokenLen)
	for idx := range parents {
		parents[idx] = make([]int, lineLen)
		for col := range parents[idx] {
			parents[idx][col] = -1
		}
	}

	prevScores := make([]int, lineLen)
	prevStarts := make([]int, lineLen)
	currScores := make([]int, lineLen)
	currStarts := make([]int, lineLen)
	fillFuzzyState(prevScores, prevStarts)
	fillFuzzyState(currScores, currStarts)

	for col := searchStart; col < lineLen; col++ {
		if !searchRunesEqual(lineRunes[col], tokenRunes[0], caseSensitive) {
			continue
		}
		prevScores[col] = fuzzyBaseScore(lineRunes, tokenRunes[0], col, caseSensitive)
		prevStarts[col] = col
	}

	for row := 1; row < tokenLen; row++ {
		fillFuzzyState(currScores, currStarts)
		bestGapScore := fuzzyNoScore
		bestGapStart := -1
		bestGapIndex := -1
		for col := searchStart; col < lineLen; col++ {
			gapCandidate := col - 2
			if gapCandidate >= searchStart && prevScores[gapCandidate] != fuzzyNoScore {
				score := prevScores[gapCandidate] + fuzzyGapPenalty*(gapCandidate+1)
				if fuzzyStateBetter(score, prevStarts[gapCandidate], gapCandidate, bestGapScore, bestGapStart, bestGapIndex) {
					bestGapScore = score
					bestGapStart = prevStarts[gapCandidate]
					bestGapIndex = gapCandidate
				}
			}

			if !searchRunesEqual(lineRunes[col], tokenRunes[row], caseSensitive) {
				continue
			}

			bestScore := fuzzyNoScore
			bestStart := -1
			bestPrev := -1

			if col-1 >= searchStart && prevScores[col-1] != fuzzyNoScore {
				score := prevScores[col-1] + fuzzyCharScore(lineRunes, tokenRunes[row], col, true, caseSensitive)
				if fuzzyStateBetter(score, prevStarts[col-1], col-1, bestScore, bestStart, bestPrev) {
					bestScore = score
					bestStart = prevStarts[col-1]
					bestPrev = col - 1
				}
			}

			if bestGapScore != fuzzyNoScore {
				score := bestGapScore - fuzzyGapPenalty*col + fuzzyCharScore(lineRunes, tokenRunes[row], col, false, caseSensitive)
				if fuzzyStateBetter(score, bestGapStart, bestGapIndex, bestScore, bestStart, bestPrev) {
					bestScore = score
					bestStart = bestGapStart
					bestPrev = bestGapIndex
				}
			}

			if bestScore == fuzzyNoScore {
				continue
			}

			currScores[col] = bestScore
			currStarts[col] = bestStart
			parents[row][col] = bestPrev
		}

		prevScores, currScores = currScores, prevScores
		prevStarts, currStarts = currStarts, prevStarts
	}

	bestEnd := -1
	bestScore := fuzzyNoScore
	bestStart := -1
	for col := searchStart; col < lineLen; col++ {
		if prevScores[col] == fuzzyNoScore {
			continue
		}
		score := finalizeFuzzyScore(prevScores[col], prevStarts[col], col, tokenLen)
		if fuzzyFinalStateBetter(score, prevStarts[col], col, bestScore, bestStart, bestEnd) {
			bestScore = score
			bestStart = prevStarts[col]
			bestEnd = col
		}
	}

	if bestEnd < 0 {
		return fuzzyTokenMatch{}, false
	}

	positions := make([]int, tokenLen)
	col := bestEnd
	for row := tokenLen - 1; row >= 0; row-- {
		positions[row] = col
		if row > 0 {
			col = parents[row][col]
			if col < 0 {
				return fuzzyTokenMatch{}, false
			}
		}
	}

	return fuzzyTokenMatch{positions: positions, score: bestScore}, true
}

func fillFuzzyState(scores, starts []int) {
	for idx := range scores {
		scores[idx] = fuzzyNoScore
		starts[idx] = -1
	}
}

func fuzzyBaseScore(lineRunes []rune, tokenRune rune, pos int, caseSensitive bool) int {
	score := 60 - pos*2
	score += fuzzyPositionBonus(lineRunes, pos)
	if !caseSensitive && lineRunes[pos] == tokenRune {
		score += 8
	}
	return score
}

func fuzzyCharScore(lineRunes []rune, tokenRune rune, pos int, adjacent bool, caseSensitive bool) int {
	score := 44
	if adjacent {
		score += 56
	}
	score += fuzzyPositionBonus(lineRunes, pos)
	if !caseSensitive && lineRunes[pos] == tokenRune {
		score += 8
	}
	return score
}

func finalizeFuzzyScore(score, start, end, tokenLen int) int {
	span := end - start + 1
	score -= span * 2
	if span == tokenLen {
		score += 60
	}
	if span <= tokenLen+2 {
		score += 18
	}
	if start == 0 {
		score += 10
	}
	return score
}

func fuzzyPositionBonus(lineRunes []rune, pos int) int {
	score := 0
	if pos == 0 {
		score += 20
	}
	if fuzzyWordBoundary(lineRunes, pos) {
		score += 18
	}
	if fuzzyCamelBoundary(lineRunes, pos) {
		score += 12
	}
	return score
}

func fuzzyWordBoundary(lineRunes []rune, pos int) bool {
	if pos <= 0 {
		return true
	}
	return !fuzzyIsWordRune(lineRunes[pos-1])
}

func fuzzyCamelBoundary(lineRunes []rune, pos int) bool {
	if pos <= 0 {
		return false
	}
	prev := lineRunes[pos-1]
	curr := lineRunes[pos]
	if unicode.IsLower(prev) && unicode.IsUpper(curr) {
		return true
	}
	return unicode.IsDigit(prev) != unicode.IsDigit(curr) && fuzzyIsWordRune(prev) && fuzzyIsWordRune(curr)
}

func fuzzyIsWordRune(r rune) bool {
	return unicode.IsLetter(r) || unicode.IsDigit(r)
}

func fuzzyStateBetter(score, start, end, bestScore, bestStart, bestEnd int) bool {
	if score != bestScore {
		return score > bestScore
	}
	span := end - start
	bestSpan := bestEnd - bestStart
	if span != bestSpan {
		return span < bestSpan
	}
	if start != bestStart {
		return start < bestStart
	}
	return end < bestEnd
}

func fuzzyFinalStateBetter(score, start, end, bestScore, bestStart, bestEnd int) bool {
	if score != bestScore {
		return score > bestScore
	}
	span := end - start
	bestSpan := bestEnd - bestStart
	if span != bestSpan {
		return span < bestSpan
	}
	if start != bestStart {
		return start < bestStart
	}
	return end < bestEnd
}

func mergeFuzzyPositions(positions []int) []NotebookSearchRange {
	if len(positions) == 0 {
		return nil
	}
	ranges := make([]NotebookSearchRange, 0, len(positions))
	current := NotebookSearchRange{Start: positions[0], End: positions[0] + 1}
	for _, pos := range positions[1:] {
		if pos <= current.End {
			if pos+1 > current.End {
				current.End = pos + 1
			}
			continue
		}
		ranges = append(ranges, current)
		current = NotebookSearchRange{Start: pos, End: pos + 1}
	}
	ranges = append(ranges, current)
	return ranges
}
