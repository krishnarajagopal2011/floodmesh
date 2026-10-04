"use client";
/** Last line of defence: a render error shows this instead of a blank page. */
export default function ErrorPage({ reset }: { error: Error; reset: () => void }) {
  return (
    <main className="adminPage narrow">
      <h1>Something went wrong</h1>
      <p>The planner hit an error while drawing this page. Your plan is still saved in this browser.</p>
      <div className="row">
        <button type="button" onClick={reset}>
          Try again
        </button>
        <a href="/">Reload the planner</a>
      </div>
    </main>
  );
}
