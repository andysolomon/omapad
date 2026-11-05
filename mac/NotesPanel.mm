#import "NotesPanel.h"

using namespace omapad;

static NSString *notesString(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string notesStd(NSString *s) { return s.UTF8String ?: ""; }

@interface NotesCell : NSTableCellView
@property(nonatomic, strong) NSTextField *previewLabel;
@end
@implementation NotesCell
@end

@interface NotesPanel () <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSTextViewDelegate>
@end

@implementation NotesPanel {
    Workspace *_workspace;
    NotesState _state;
    BOOL _syncing;
    NSSplitView *_layout, *_writing;
    NSView *_sidebar, *_source, *_empty;
    NSTableView *_list;
    NSScrollView *_listScroll, *_previewScroll;
    NSSearchField *_search;
    NSPopUpButton *_tags;
    NSSegmentedControl *_views;
    NSButton *_focusButton, *_retry;
    NSTextField *_libraryTitle, *_libraryCount, *_listEmpty, *_status, *_words;
    NSTextView *_preview;
    NSMutableArray<NSButton *> *_formatButtons;
    NSArray<NSString *> *_tagKeys;
    std::string _rowsFingerprint, _tagsFingerprint, _previewFingerprint;
    BOOL _positioned;
}

- (NSButton *)button:(NSString *)title symbol:(NSString *)symbol action:(SEL)action {
    NSButton *button = [NSButton buttonWithTitle:title target:self action:action];
    if (symbol.length) {
        button.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
        button.imagePosition = title.length ? NSImageLeft : NSImageOnly;
    }
    button.bezelStyle = NSBezelStyleInline;
    button.refusesFirstResponder = YES;
    return button;
}

- (NSStackView *)row:(NSArray<NSView *> *)views {
    NSStackView *row = [NSStackView stackViewWithViews:views];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.spacing = 8;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    return row;
}

- (instancetype)initWithWorkspace:(Workspace *)workspace {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 1100, 700)])) {
        _workspace = workspace;
        _formatButtons = [NSMutableArray array];
        self.translatesAutoresizingMaskIntoConstraints = NO;
        NSButton *back = [self button:@"Editor" symbol:@"chevron.left" action:@selector(back:)];
        back.toolTip = @"Return to the editor; notes stay saved";
        _focusButton = [self button:@"" symbol:@"sidebar.left" action:@selector(toggleFocus:)];
        _focusButton.toolTip = @"Show or hide the notes library";
        _focusButton.accessibilityLabel = @"Toggle notes library";
        NSTextField *title = [NSTextField labelWithString:@"NOTES"];
        title.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
        title.textColor = NSColor.secondaryLabelColor;
        _views = [NSSegmentedControl segmentedControlWithLabels:@[@"Write", @"Split", @"Read"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(viewChanged:)];
        _views.selectedSegment = 0;
        _views.refusesFirstResponder = YES;
        _views.accessibilityLabel = @"Notes view mode";
        NSButton *newNote = [self button:@"New Note" symbol:@"square.and.pencil" action:@selector(newNote:)];
        NSView *spacer = [NSView new];
        [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSStackView *top = [self row:@[back, _focusButton, title, spacer, _views, newNote]];
        top.edgeInsets = NSEdgeInsetsMake(0, 12, 0, 12);

        NSMutableArray<NSView *> *format = [NSMutableArray array];
        NSArray *titles = @[@"H", @"B", @"I", @"•", @"☐", @"❯", @"</>", @"Link"];
        NSArray *actions = @[@"heading", @"bold", @"italic", @"bullet", @"task", @"quote", @"code", @"link"];
        NSArray *tips = @[@"Heading", @"Bold", @"Italic", @"Bullet list", @"Task list", @"Quote", @"Inline code", @"Insert link"];
        for (NSUInteger i = 0; i < titles.count; i++) {
            NSButton *button = [self button:titles[i] symbol:nil action:@selector(format:)];
            button.identifier = actions[i];
            button.toolTip = tips[i];
            button.accessibilityLabel = tips[i];
            button.font = [NSFont systemFontOfSize:13 weight:i == 1 ? NSFontWeightBold : NSFontWeightRegular];
            [button.widthAnchor constraintGreaterThanOrEqualToConstant:28].active = YES;
            [_formatButtons addObject:button];
            [format addObject:button];
        }
        NSView *formatSpacer = [NSView new];
        [formatSpacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [format addObject:formatSpacer];
        _status = [NSTextField labelWithString:@""];
        _status.font = [NSFont systemFontOfSize:11];
        _status.textColor = NSColor.secondaryLabelColor;
        _status.lineBreakMode = NSLineBreakByTruncatingTail;
        [_status setContentCompressionResistancePriority:200 forOrientation:NSLayoutConstraintOrientationHorizontal];
        _retry = [self button:@"Retry" symbol:@"arrow.clockwise" action:@selector(retry:)];
        _retry.hidden = YES;
        _words = [NSTextField labelWithString:@"0 words"];
        _words.font = [NSFont systemFontOfSize:11];
        _words.textColor = NSColor.secondaryLabelColor;
        [format addObjectsFromArray:@[_status, _retry, _words]];
        NSStackView *formatRow = [self row:format];
        formatRow.edgeInsets = NSEdgeInsetsMake(0, 12, 0, 12);
        NSBox *rule = [NSBox new];
        rule.boxType = NSBoxSeparator;
        rule.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:top];
        [self addSubview:formatRow];
        [self addSubview:rule];

        [self buildSidebar];
        [self buildPreview];
        _writing = [NSSplitView new];
        _writing.vertical = YES;
        _writing.dividerStyle = NSSplitViewDividerStyleThin;
        _empty = [NSView new];
        NSTextField *emptyTitle = [NSTextField labelWithString:@"A place for your next idea"];
        emptyTitle.font = [NSFont systemFontOfSize:22 weight:NSFontWeightSemibold];
        NSTextField *emptyDetail = [NSTextField wrappingLabelWithString:@"Create a note and start typing. Your notes save automatically as Markdown files."];
        emptyDetail.alignment = NSTextAlignmentCenter;
        emptyDetail.textColor = NSColor.secondaryLabelColor;
        NSButton *emptyNew = [NSButton buttonWithTitle:@"New Note" target:self action:@selector(newNote:)];
        emptyNew.bezelStyle = NSBezelStyleRounded;
        for (NSView *v in @[emptyTitle, emptyDetail, emptyNew]) { v.translatesAutoresizingMaskIntoConstraints = NO; [_empty addSubview:v]; }
        [NSLayoutConstraint activateConstraints:@[
            [emptyTitle.centerXAnchor constraintEqualToAnchor:_empty.centerXAnchor],
            [emptyTitle.centerYAnchor constraintEqualToAnchor:_empty.centerYAnchor constant:-40],
            [emptyDetail.topAnchor constraintEqualToAnchor:emptyTitle.bottomAnchor constant:14],
            [emptyDetail.centerXAnchor constraintEqualToAnchor:_empty.centerXAnchor],
            [emptyDetail.widthAnchor constraintLessThanOrEqualToConstant:320],
            [emptyDetail.widthAnchor constraintLessThanOrEqualToAnchor:_empty.widthAnchor multiplier:0.85],
            [emptyNew.topAnchor constraintEqualToAnchor:emptyDetail.bottomAnchor constant:20],
            [emptyNew.centerXAnchor constraintEqualToAnchor:_empty.centerXAnchor],
        ]];
        _layout = [NSSplitView new];
        _layout.vertical = YES;
        _layout.dividerStyle = NSSplitViewDividerStyleThin;
        _layout.translatesAutoresizingMaskIntoConstraints = NO;
        [_layout addArrangedSubview:_sidebar];
        [_layout addArrangedSubview:_writing];
        [self addSubview:_layout];
        [NSLayoutConstraint activateConstraints:@[
            [top.topAnchor constraintEqualToAnchor:self.topAnchor],
            [top.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [top.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [top.heightAnchor constraintEqualToConstant:42],
            [formatRow.topAnchor constraintEqualToAnchor:top.bottomAnchor],
            [formatRow.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [formatRow.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [formatRow.heightAnchor constraintEqualToConstant:34],
            [rule.topAnchor constraintEqualToAnchor:formatRow.bottomAnchor],
            [rule.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [rule.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [rule.heightAnchor constraintEqualToConstant:1],
            [_layout.topAnchor constraintEqualToAnchor:rule.bottomAnchor],
            [_layout.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [_layout.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [_layout.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        ]];
    }
    return self;
}

- (void)buildSidebar {
    _sidebar = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 270, 600)];
    _libraryTitle = [NSTextField labelWithString:@"All Notes"];
    _libraryTitle.font = [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold];
    _libraryCount = [NSTextField labelWithString:@"0 notes"];
    _libraryCount.font = [NSFont systemFontOfSize:11];
    _libraryCount.textColor = NSColor.secondaryLabelColor;
    _search = [NSSearchField new];
    _search.placeholderString = @"Search all notes";
    _search.accessibilityLabel = @"Search notes";
    _search.delegate = self;
    _search.sendsSearchStringImmediately = YES;
    _tags = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _tags.target = self;
    _tags.action = @selector(tagChanged:);
    _tags.accessibilityLabel = @"Filter notes by tag";
    _tags.refusesFirstResponder = YES;
    _list = [NSTableView new];
    _list.headerView = nil;
    _list.rowHeight = 66;
    _list.intercellSpacing = NSMakeSize(0, 1);
    _list.allowsEmptySelection = YES;
    _list.allowsMultipleSelection = NO;
    _list.dataSource = self;
    _list.delegate = self;
    _list.style = NSTableViewStyleSourceList;
    _list.accessibilityLabel = @"Notes";
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"note"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [_list addTableColumn:column];
    _list.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _listScroll = [NSScrollView new];
    _listScroll.documentView = _list;
    _listScroll.hasVerticalScroller = YES;
    _listScroll.autohidesScrollers = YES;
    _listEmpty = [NSTextField wrappingLabelWithString:@"No notes yet. Click New Note to begin."];
    _listEmpty.textColor = NSColor.secondaryLabelColor;
    _listEmpty.font = [NSFont systemFontOfSize:12];
    _listEmpty.alignment = NSTextAlignmentCenter;
    NSButton *folder = [self button:@"Folder…" symbol:@"folder" action:@selector(folder:)];
    folder.toolTip = @"Choose a folder of Markdown notes";
    NSButton *importButton = [self button:@"Import…" symbol:@"square.and.arrow.down" action:@selector(import:)];
    NSButton *more = [self button:@"•••" symbol:nil action:@selector(more:)];
    more.accessibilityLabel = @"More note actions";
    NSStackView *footer = [self row:@[folder, importButton, more]];
    footer.distribution = NSStackViewDistributionFillProportionally;
    for (NSView *v in @[_libraryTitle, _libraryCount, _search, _tags, _listScroll, _listEmpty, footer]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [_sidebar addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [_libraryTitle.topAnchor constraintEqualToAnchor:_sidebar.topAnchor constant:18],
        [_libraryTitle.leadingAnchor constraintEqualToAnchor:_sidebar.leadingAnchor constant:16],
        [_libraryTitle.trailingAnchor constraintLessThanOrEqualToAnchor:_sidebar.trailingAnchor constant:-12],
        [_libraryCount.topAnchor constraintEqualToAnchor:_libraryTitle.bottomAnchor constant:4],
        [_libraryCount.leadingAnchor constraintEqualToAnchor:_libraryTitle.leadingAnchor],
        [_search.topAnchor constraintEqualToAnchor:_libraryCount.bottomAnchor constant:16],
        [_search.leadingAnchor constraintEqualToAnchor:_sidebar.leadingAnchor constant:12],
        [_search.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor constant:-12],
        [_tags.topAnchor constraintEqualToAnchor:_search.bottomAnchor constant:8],
        [_tags.leadingAnchor constraintEqualToAnchor:_search.leadingAnchor],
        [_tags.trailingAnchor constraintEqualToAnchor:_search.trailingAnchor],
        [_listScroll.topAnchor constraintEqualToAnchor:_tags.bottomAnchor constant:12],
        [_listScroll.leadingAnchor constraintEqualToAnchor:_sidebar.leadingAnchor],
        [_listScroll.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor],
        [_listScroll.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-8],
        [footer.leadingAnchor constraintEqualToAnchor:_sidebar.leadingAnchor constant:12],
        [footer.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor constant:-12],
        [footer.bottomAnchor constraintEqualToAnchor:_sidebar.bottomAnchor constant:-12],
        [footer.heightAnchor constraintEqualToConstant:24],
        [_listEmpty.topAnchor constraintEqualToAnchor:_listScroll.topAnchor constant:28],
        [_listEmpty.leadingAnchor constraintEqualToAnchor:_sidebar.leadingAnchor constant:18],
        [_listEmpty.trailingAnchor constraintEqualToAnchor:_sidebar.trailingAnchor constant:-18],
    ]];
}

- (void)buildPreview {
    _preview = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 450, 600)];
    _preview.editable = NO;
    _preview.selectable = YES;
    _preview.richText = YES;
    _preview.importsGraphics = NO;
    _preview.automaticLinkDetectionEnabled = NO;
    _preview.delegate = self;
    _preview.drawsBackground = YES;
    _preview.backgroundColor = NSColor.textBackgroundColor;
    _preview.textContainerInset = NSMakeSize(28, 24);
    _preview.verticallyResizable = YES;
    _preview.horizontallyResizable = NO;
    _preview.autoresizingMask = NSViewWidthSizable;
    _preview.textContainer.widthTracksTextView = YES;
    _preview.textContainer.containerSize = NSMakeSize(450, CGFLOAT_MAX);
    _preview.accessibilityLabel = @"Markdown preview";
    _previewScroll = [NSScrollView new];
    _previewScroll.documentView = _preview;
    _previewScroll.hasVerticalScroller = YES;
    _previewScroll.autohidesScrollers = YES;
}

- (void)setSourceView:(NSView *)view {
    if (_source == view) return;
    [_source removeFromSuperview];
    _source = view;
    [self updateWritingLayout];
}
- (void)focusPreview { [self.window makeFirstResponder:_preview]; }

- (NSView *)detachSourceView {
    NSView *view = _source;
    [_source removeFromSuperview];
    _source = nil;
    return view;
}

- (void)updateWritingLayout {
    NSMutableArray<NSView *> *wanted = [NSMutableArray array];
    if (!_state.activeNote) [wanted addObject:_empty];
    else {
        if (_state.viewMode != 2 && _source) [wanted addObject:_source];
        if (_state.viewMode != 0) [wanted addObject:_previewScroll];
    }
    const BOOL changed = ![_writing.arrangedSubviews isEqualToArray:wanted];
    if (changed) {
        for (NSView *view in [_writing.arrangedSubviews copy]) [view removeFromSuperview];
        for (NSView *view in wanted) [_writing addArrangedSubview:view];
        [_writing adjustSubviews];
    }
    if (changed && wanted.count == 2) {
        [_writing layoutSubtreeIfNeeded];
        [_writing setPosition:NSWidth(_writing.bounds) / 2 ofDividerAtIndex:0];
    }
    const BOOL sidebarVisible = _sidebar.superview != nil;
    if (_state.focus && sidebarVisible) [_sidebar removeFromSuperview];
    else if (!_state.focus && !sidebarVisible) {
        [_layout insertArrangedSubview:_sidebar atIndex:0];
        [_layout layoutSubtreeIfNeeded];
        [_layout setPosition:270 ofDividerAtIndex:0];
    }
}

- (void)layout {
    [super layout];
    if (!_positioned && NSWidth(_layout.bounds) > 500) {
        _positioned = YES;
        if (_sidebar.superview) [_layout setPosition:270 ofDividerAtIndex:0];
        if (_writing.arrangedSubviews.count == 2) [_writing setPosition:NSWidth(_writing.bounds) / 2 ofDividerAtIndex:0];
    }
}

- (void)updateState:(const NotesState &)state {
    _syncing = YES;
    const std::string oldPath = _state.selectedPath;
    _state = state;
    self.hidden = !state.enabled;
    _views.selectedSegment = state.viewMode;
    _views.enabled = state.activeNote;
    _focusButton.state = state.focus ? NSControlStateValueOn : NSControlStateValueOff;
    _libraryTitle.stringValue = state.tag.empty() ? @"All Notes" : [@"#" stringByAppendingString:notesString(state.tag)];
    _libraryTitle.toolTip = notesString(state.folder);
    _libraryCount.stringValue = [NSString stringWithFormat:@"%lu of %lu notes", state.notes.size(), state.total];
    if (![_search.stringValue isEqualToString:notesString(state.query)]) _search.stringValue = notesString(state.query);
    _status.stringValue = notesString(state.status);
    _status.toolTip = notesString(state.status);
    _status.textColor = state.saveError ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    _retry.hidden = !state.saveError;
    _words.stringValue = [NSString stringWithFormat:@"%lu %@", state.words, state.words == 1 ? @"word" : @"words"];
    for (NSButton *b in _formatButtons) b.enabled = state.activeNote && state.viewMode != 2;
    std::string tagFingerprint;
    for (const auto &tag : state.tags) tagFingerprint += tag.first + "\n" + std::to_string(tag.second) + "\n";
    if (tagFingerprint != _tagsFingerprint || !_tagKeys) {
        _tagsFingerprint = tagFingerprint;
        [_tags removeAllItems];
        [_tags addItemWithTitle:@"All tags"];
        NSMutableArray<NSString *> *keys = [NSMutableArray arrayWithObject:@""];
        for (const auto &tag : state.tags) {
            [_tags addItemWithTitle:[NSString stringWithFormat:@"#%@  ·  %d", notesString(tag.first), tag.second]];
            [keys addObject:notesString(tag.first)];
        }
        _tagKeys = keys;
    }
    const NSUInteger selectedTag = [_tagKeys indexOfObject:notesString(state.tag)];
    [_tags selectItemAtIndex:selectedTag == NSNotFound ? 0 : selectedTag];
    std::string rowsFingerprint;
    for (const NoteSummary &note : state.notes) rowsFingerprint += note.path + "\n" + note.title + "\n" + note.preview + "\n";
    NSInteger selected = -1;
    for (size_t i = 0; i < state.notes.size(); i++) if (state.notes[i].path == state.selectedPath) selected = static_cast<NSInteger>(i);
    if (rowsFingerprint != _rowsFingerprint) {
        _rowsFingerprint = rowsFingerprint;
        const NSPoint origin = _listScroll.contentView.bounds.origin;
        [_list reloadData];
        [_listScroll.contentView scrollToPoint:origin];
        [_listScroll reflectScrolledClipView:_listScroll.contentView];
    }
    if (selected >= 0) {
        [_list selectRowIndexes:[NSIndexSet indexSetWithIndex:selected] byExtendingSelection:NO];
        if (oldPath != state.selectedPath) [_list scrollRowToVisible:selected];
    } else [_list deselectAll:nil];
    _listEmpty.hidden = !state.notes.empty();
    _listEmpty.stringValue = state.total == 0 ? @"No notes yet. Click New Note to begin." : @"No matching notes. Try another search or tag.";
    [self renderPreview:state];
    [self updateWritingLayout];
    _syncing = NO;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { return _state.notes.size(); }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    if (row < 0 || static_cast<size_t>(row) >= _state.notes.size()) return nil;
    NotesCell *cell = [table makeViewWithIdentifier:@"note" owner:self];
    if (!cell) {
        cell = [NotesCell new];
        cell.identifier = @"note";
        NSTextField *title = [NSTextField labelWithString:@""];
        title.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        NSTextField *preview = [NSTextField labelWithString:@""];
        preview.font = [NSFont systemFontOfSize:11];
        preview.textColor = NSColor.secondaryLabelColor;
        preview.lineBreakMode = NSLineBreakByTruncatingTail;
        title.translatesAutoresizingMaskIntoConstraints = NO;
        preview.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:title];
        [cell addSubview:preview];
        cell.textField = title;
        cell.previewLabel = preview;
        [NSLayoutConstraint activateConstraints:@[
            [title.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:10],
            [title.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-10],
            [title.topAnchor constraintEqualToAnchor:cell.topAnchor constant:12],
            [preview.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
            [preview.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
            [preview.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:6],
        ]];
    }
    const NoteSummary &note = _state.notes[row];
    cell.textField.stringValue = notesString(note.title);
    cell.previewLabel.stringValue = note.preview.empty() ? @"Empty note" : notesString(note.preview);
    cell.toolTip = notesString(note.path);
    return cell;
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    if (_syncing) return;
    const NSInteger row = _list.selectedRow;
    if (row >= 0 && static_cast<size_t>(row) < _state.notes.size()) _workspace->openNote(_state.notes[row].path);
}
- (void)controlTextDidChange:(NSNotification *)notification {
    if (!_syncing && notification.object == _search) _workspace->setNotesQuery(notesStd(_search.stringValue));
}
- (void)tagChanged:(id)sender {
    if (_syncing) return;
    const NSInteger index = _tags.indexOfSelectedItem;
    if (index >= 0 && static_cast<NSUInteger>(index) < _tagKeys.count) _workspace->setNotesTag(notesStd(_tagKeys[index]));
}

- (void)renderPreview:(const NotesState &)state {
    std::string fingerprint = state.selectedPath;
    for (const NoteBlock &block : state.preview) {
        fingerprint += std::to_string(static_cast<int>(block.kind)) + ":" + std::to_string(block.level) + (block.checked ? "1" : "0");
        for (const NoteSpan &span : block.spans) fingerprint += std::to_string(span.style) + ":" + span.text + "\n" + span.href;
    }
    if (fingerprint == _previewFingerprint) return;
    _previewFingerprint = fingerprint;
    NSMutableAttributedString *text = [NSMutableAttributedString new];
    for (const NoteBlock &block : state.preview) {
        NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new];
        paragraph.lineSpacing = 5;
        paragraph.paragraphSpacing = 14;
        const BOOL codeBlock = block.kind == NoteBlock::Kind::Code;
        const BOOL heading = block.kind == NoteBlock::Kind::Heading;
        CGFloat size = heading ? MAX(17, 30 - block.level * 3) : 15;
        NSFont *base = codeBlock ? [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular] : [NSFont systemFontOfSize:size weight:heading ? NSFontWeightSemibold : NSFontWeightRegular];
        NSDictionary *baseAttrs = @{NSFontAttributeName:base, NSForegroundColorAttributeName:NSColor.labelColor, NSParagraphStyleAttributeName:paragraph};
        NSString *prefix = @"";
        if (block.kind == NoteBlock::Kind::Task) prefix = block.checked ? @"☑  " : @"☐  ";
        else if (block.kind == NoteBlock::Kind::List) prefix = @"•  ";
        else if (block.kind == NoteBlock::Kind::Quote) prefix = @"│  ";
        else if (block.kind == NoteBlock::Kind::Rule) prefix = @"────────────────────";
        if (prefix.length) [text appendAttributedString:[[NSAttributedString alloc] initWithString:prefix attributes:@{NSFontAttributeName:base, NSForegroundColorAttributeName:NSColor.controlAccentColor, NSParagraphStyleAttributeName:paragraph}]];
        for (const NoteSpan &span : block.spans) {
            NSMutableDictionary *attrs = [baseAttrs mutableCopy];
            NSFont *font = base;
            if (span.style & 4) font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
            if (span.style & 1) font = [NSFontManager.sharedFontManager convertFont:font toHaveTrait:NSBoldFontMask];
            if (span.style & 2) font = [NSFontManager.sharedFontManager convertFont:font toHaveTrait:NSItalicFontMask];
            attrs[NSFontAttributeName] = font;
            if (codeBlock || (span.style & 4)) attrs[NSBackgroundColorAttributeName] = NSColor.quaternaryLabelColor;
            if (span.style & 8) {
                NSURLComponents *url = [NSURLComponents componentsWithString:notesString(span.href)];
                if (([url.scheme.lowercaseString isEqualToString:@"https"] || [url.scheme.lowercaseString isEqualToString:@"http"]) && url.host.length) {
                    attrs[NSLinkAttributeName] = url.URL;
                    attrs[NSForegroundColorAttributeName] = NSColor.linkColor;
                }
            }
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:notesString(span.text) attributes:attrs]];
        }
        [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:baseAttrs]];
    }
    if (!text.length) [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"Your Markdown preview appears here as you write." attributes:@{NSFontAttributeName:[NSFont systemFontOfSize:15], NSForegroundColorAttributeName:NSColor.secondaryLabelColor}]];
    const NSPoint origin = _previewScroll.contentView.bounds.origin;
    const NSRange selection = _preview.selectedRange;
    [_preview.textStorage setAttributedString:text];
    if (selection.location <= text.length && selection.length <= text.length - selection.location) [_preview setSelectedRange:selection];
    [_previewScroll.contentView scrollToPoint:origin];
    [_previewScroll reflectScrolledClipView:_previewScroll.contentView];
}
- (BOOL)textView:(NSTextView *)textView clickedOnLink:(id)link atIndex:(NSUInteger)charIndex {
    NSURL *url = [link isKindOfClass:NSURL.class] ? link : [NSURL URLWithString:[link description]];
    if (([url.scheme.lowercaseString isEqualToString:@"https"] || [url.scheme.lowercaseString isEqualToString:@"http"]) && url.host.length) [NSWorkspace.sharedWorkspace openURL:url];
    return YES;
}

- (void)back:(id)sender { _workspace->run(Cmd::NotesMode); }
- (void)newNote:(id)sender { _workspace->run(Cmd::NotesNew); }
- (void)folder:(id)sender { _workspace->run(Cmd::NotesFolder); }
- (void)import:(id)sender { _workspace->run(Cmd::NotesImport); }
- (void)retry:(id)sender { _workspace->run(Cmd::NotesRetry); }
- (void)toggleFocus:(id)sender { _workspace->run(Cmd::NotesFocus); }
- (void)viewChanged:(id)sender { if (!_syncing) _workspace->run(Cmd::NotesView, static_cast<int>(_views.selectedSegment)); }
- (void)format:(NSButton *)sender { _workspace->run(Cmd::NotesFormat, 0, notesStd(sender.identifier)); }
- (void)runMenu:(NSMenuItem *)item { _workspace->run(static_cast<Cmd>(item.tag)); }
- (void)more:(NSButton *)sender {
    NSMenu *menu = [NSMenu new];
    menu.autoenablesItems = NO;
    for (const auto &entry : std::vector<std::pair<const char *, Cmd>>{{"Show Notes Folder", Cmd::NotesReveal}, {"Refresh Notes", Cmd::NotesRefresh}, {"Export as HTML…", Cmd::NotesExportHtml}, {"Move Note to Trash…", Cmd::NotesTrash}}) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:entry.first] action:@selector(runMenu:) keyEquivalent:@""];
        item.target = self;
        item.tag = static_cast<NSInteger>(entry.second);
        if (entry.second == Cmd::NotesTrash || entry.second == Cmd::NotesExportHtml) item.enabled = _state.activeNote;
        [menu addItem:item];
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, NSHeight(sender.bounds)) inView:sender];
}
@end
